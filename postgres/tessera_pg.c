#include "postgres.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <unistd.h>

#include "access/stratnum.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "access/attmap.h"
#include "access/sysattr.h"
#include "catalog/pg_inherits.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "common/file_utils.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "portability/instr_time.h"
#include <x86intrin.h>
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planner.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rls.h"
#include "utils/selfuncs.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/varlena.h"

#include "tessera_c.h"

PG_MODULE_MAGIC;

void _PG_init(void);

static bool tessera_enabled = true;
static int tessera_threads = 0;

static int
query_threads(int requested)
{
	return requested > 0 ? requested : max_parallel_workers_per_gather + 1;
}
static int tessera_frontier = -1;
static bool tessera_verbose = false;
static double tessera_min_expansion = 0;
static double tessera_min_expansion_cross = 0;
static create_upper_paths_hook_type prev_upper_hook = NULL;

enum {
	P_RELID, P_KATT, P_XATT, P_VATT, P_KTYPE, P_XTYPE, P_VTYPE, P_MODE, P_COL0_SMALLER, P_AGGTYPE, P_MINVAL,
	P_DISTINCT, P_TOPK,
	P_XCOLL,
	P_CROSS, P_RELID2, P_KATT2, P_XATT2, P_VATT2, P_KTYPE2, P_XTYPE2, P_VTYPE2,
	P_GROUP_TOPK, P_GROUP_OF, P_NPARAM
};

#define TESSERA_MAXSUMS 16
#define TESSERA_MAXFACTORS 4

typedef struct TesseraMatch {
	int64 p[P_NPARAM];
	Aggref *agg;
	int va;
	int vb;
} TesseraMatch;

static Var *as_var(Node *n) {
	while (n && IsA(n, RelabelType)) {
		n = (Node *)((RelabelType *)n)->arg;
	}
	if (n && IsA(n, Var) && ((Var *)n)->varlevelsup == 0 && ((Var *)n)->varattno > 0) {
		return (Var *)n;
	}
	return NULL;
}

static int64 datum_int(Datum d, Oid t);

static bool att_not_null_one(Oid relid, AttrNumber att) {
	Relation r = table_open(relid, NoLock);
	TupleDesc d = RelationGetDescr(r);
	bool nn = att > 0 && att <= d->natts && !TupleDescAttr(d, att - 1)->attisdropped &&
	          TupleDescAttr(d, att - 1)->attgenerated != ATTRIBUTE_GENERATED_VIRTUAL &&
	          TupleDescCompactAttr(d, att - 1)->attnullability == ATTNULLABLE_VALID;
	table_close(r, NoLock);
	return nn;
}

static bool att_not_null(Oid relid, AttrNumber att) {
	if (!OidIsValid(relid)) {
		return false;
	}
	if (!att_not_null_one(relid, att)) {
		return false;
	}
	if (!has_subclass(relid)) {
		return true;
	}
	char *name = get_attname(relid, att, true);
	if (!name) {
		return false;
	}
	ListCell *lc;
	foreach (lc, find_all_inheritors(relid, AccessShareLock, NULL)) {
		const Oid child = lfirst_oid(lc);
		if (child != relid && !att_not_null_one(child, get_attnum(child, name))) {
			return false;
		}
	}
	return true;
}

static const char *catalog_opname(Oid opno) {
	HeapTuple tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(opno));
	if (!HeapTupleIsValid(tup)) {
		return NULL;
	}
	Form_pg_operator f = (Form_pg_operator)GETSTRUCT(tup);
	const char *name = f->oprnamespace == PG_CATALOG_NAMESPACE ? pstrdup(NameStr(f->oprname)) : NULL;
	ReleaseSysCache(tup);
	return name;
}

static bool int_type(Oid t) {
	return t == INT2OID || t == INT4OID || t == INT8OID;
}

static bool str_type(Oid t) {
	return t == TEXTOID || t == VARCHAROID;
}

static bool text_type(Oid t) {
	return t == NUMERICOID || t == BOOLOID || t == UUIDOID;
}

static bool text_key_ok(Oid t, int32 typmod) {
	return text_type(t) && (t != NUMERICOID || typmod >= (int32)VARHDRSZ);
}

static bool bytes_type(Oid t) {
	return str_type(t) || text_type(t);
}

static void value_bytes(Datum d, Oid t, const char **p, uint32 *len) {
	if (str_type(t)) {
		text *v = DatumGetTextPP(d);
		*p = VARDATA_ANY(v);
		*len = VARSIZE_ANY_EXHDR(v);
		return;
	}
	Oid out;
	bool varlena;
	getTypeOutputInfo(t, &out, &varlena);
	*p = OidOutputFunctionCall(out, d);
	*len = (uint32)strlen(*p);
}

static bool numeric_typmod(int32 typmod, int *prec, int *scale);

static Var *cast_var(Node *n, bool *cast) {
	*cast = false;
	if (n && IsA(n, FuncExpr) && ((FuncExpr *)n)->funcresulttype == FLOAT8OID && list_length(((FuncExpr *)n)->args) == 1 &&
	    get_func_namespace(((FuncExpr *)n)->funcid) == PG_CATALOG_NAMESPACE) {
		const char *name = get_func_name(((FuncExpr *)n)->funcid);
		Var *v = as_var((Node *)linitial(((FuncExpr *)n)->args));
		int p, sc;
		if (v && name && !strcmp(name, "float8") &&
		    (int_type(v->vartype) ||
		     (v->vartype == NUMERICOID && numeric_typmod(v->vartypmod, &p, &sc) && p <= 18))) {
			*cast = true;
			return v;
		}
		return NULL;
	}
	return as_var(n);
}

static bool temporal_type(Oid t) {
	return t == DATEOID || t == TIMESTAMPOID || t == TIMESTAMPTZOID;
}

static bool key_ok(Oid t, Oid coll) {
	return int_type(t) || temporal_type(t) || (str_type(t) && OidIsValid(coll) && get_collation_isdeterministic(coll));
}

static bool keys_compatible(Oid a, Oid b) {
	return (int_type(a) && int_type(b)) || (str_type(a) && str_type(b)) || (temporal_type(a) && a == b);
}

static bool binop_cross(Node *n, int vx, int vy, const char **op, Var **x, Var **y, Oid *coll) {
	if (!IsA(n, OpExpr) || list_length(((OpExpr *)n)->args) != 2) {
		return false;
	}
	OpExpr *o = (OpExpr *)n;
	Var *l = as_var(linitial(o->args)), *r = as_var(lsecond(o->args));
	if (!l || !r || !((l->varno == vx && r->varno == vy) || (l->varno == vy && r->varno == vx))) {
		return false;
	}
	*op = catalog_opname(o->opno);
	*x = l->varno == vx ? l : r;
	*y = l->varno == vx ? r : l;
	*coll = o->inputcollid;
	return *op != NULL;
}

static bool binop_vars(Node *n, int va, int vb, const char **op, Var **left, Var **right, Oid *coll) {
	if (!IsA(n, OpExpr) || list_length(((OpExpr *)n)->args) != 2) {
		return false;
	}
	OpExpr *o = (OpExpr *)n;
	Var *l = as_var(linitial(o->args)), *r = as_var(lsecond(o->args));
	if (!l || !r || l->varattno != r->varattno || !((l->varno == va && r->varno == vb) || (l->varno == vb && r->varno == va))) {
		return false;
	}
	*op = catalog_opname(o->opno);
	*left = l;
	*right = r;
	*coll = o->inputcollid;
	return *op != NULL;
}

static bool match_cross(PlannerInfo *root, TesseraMatch *m, Var *g0, Var *g1, List *tl);
static bool match_agg(PlannerInfo *root, TesseraMatch *m, int va, int vb, Var *g0, Var *g1, List *tl);

static List *group_tlist(Query *parse, RelOptInfo *output_rel) {
	if (!parse->hasWindowFuncs) {
		return parse->targetList;
	}
	List *tl = NIL;
	int resno = 1;
	ListCell *lc;
	foreach (lc, output_rel->reltarget->exprs) {
		tl = lappend(tl, makeTargetEntry((Expr *)lfirst(lc), (AttrNumber)resno++, NULL, false));
	}
	return tl;
}

static Node *join_input(Node *n, List **quals) {
	while (IsA(n, FromExpr) && list_length(((FromExpr *)n)->fromlist) == 1) {
		*quals = list_concat_copy((List *)((FromExpr *)n)->quals, *quals);
		n = (Node *)linitial(((FromExpr *)n)->fromlist);
	}
	return n;
}

static bool join_inputs(Query *parse, int *va, int *vb, List **quals) {
	List *from = parse->jointree->fromlist;
	*quals = (List *)parse->jointree->quals;
	Node *a = NULL, *b = NULL;
	if (list_length(from) == 1) {
		Node *n = join_input((Node *)linitial(from), quals);
		if (IsA(n, FromExpr) && list_length(((FromExpr *)n)->fromlist) == 2) {
			*quals = list_concat_copy((List *)((FromExpr *)n)->quals, *quals);
			from = ((FromExpr *)n)->fromlist;
		} else if (IsA(n, JoinExpr) && ((JoinExpr *)n)->jointype == JOIN_INNER) {
			*quals = list_concat_copy((List *)((JoinExpr *)n)->quals, *quals);
			a = ((JoinExpr *)n)->larg;
			b = ((JoinExpr *)n)->rarg;
		} else {
			return false;
		}
	}
	if (!a) {
		if (list_length(from) != 2) {
			return false;
		}
		a = (Node *)linitial(from);
		b = (Node *)lsecond(from);
	}
	a = join_input(a, quals);
	b = join_input(b, quals);
	if (!IsA(a, RangeTblRef) || !IsA(b, RangeTblRef)) {
		return false;
	}
	*va = ((RangeTblRef *)a)->rtindex;
	*vb = ((RangeTblRef *)b)->rtindex;
	return true;
}

static bool match_query(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *output_rel, TesseraMatch *m) {
	Query *parse = root->parse;
	int va, vb;
	List *quals;
	const bool distinct_form = parse->groupClause == NIL && list_length(parse->distinctClause) == 2 &&
	                           !parse->hasDistinctOn && !parse->hasAggs && !parse->havingQual;
	List *gc = distinct_form ? parse->distinctClause : parse->groupClause;
	if (stage != (distinct_form ? UPPERREL_DISTINCT : UPPERREL_GROUP_AGG) || parse->commandType != CMD_SELECT ||
	    parse->setOperations || parse->groupingSets || (parse->hasWindowFuncs && distinct_form) || parse->hasTargetSRFs ||
	    (parse->distinctClause && !distinct_form) || parse->hasSubLinks || list_length(gc) != 2 ||
	    !join_inputs(parse, &va, &vb, &quals)) {
		return false;
	}
	RangeTblEntry *ra = rt_fetch(va, parse->rtable), *rb = rt_fetch(vb, parse->rtable);
	if (ra->rtekind != RTE_RELATION || rb->rtekind != RTE_RELATION || ra->relkind != RELKIND_RELATION ||
	    rb->relkind != RELKIND_RELATION || ra->tablesample || rb->tablesample || has_subclass(ra->relid) ||
	    has_subclass(rb->relid)) {
		return false;
	}
	if (parse->hasRowSecurity || ra->securityQuals || rb->securityQuals ||
	    check_enable_rls(ra->relid, InvalidOid, true) != RLS_NONE ||
	    check_enable_rls(rb->relid, InvalidOid, true) != RLS_NONE) {
		return false;
	}
	memset(m, 0, sizeof *m);
	m->p[P_RELID] = ra->relid;
	m->va = va;
	m->vb = vb;
	Var *g0 = as_var((Node *)get_sortgroupclause_expr(linitial_node(SortGroupClause, gc), parse->targetList));
	Var *g1 = as_var((Node *)get_sortgroupclause_expr(lsecond_node(SortGroupClause, gc), parse->targetList));
	if (!g0 || !g1 || g0->varno == g1->varno ||
	    !((g0->varno == va || g0->varno == vb) && (g1->varno == va || g1->varno == vb))) {
		return false;
	}
	for (int i = 0; i < 2; i++) {
		Node *e = (Node *)get_sortgroupclause_expr(list_nth_node(SortGroupClause, gc, i), parse->targetList);
		Oid c = exprCollation(e);
		if (str_type(exprType(e)) && !(OidIsValid(c) && get_collation_isdeterministic(c))) {
			return false;
		}
	}
	if (ra->relid != rb->relid || g0->varattno != g1->varattno) {
		return match_cross(root, m, g0, g1, group_tlist(parse, output_rel));
	}
	if (!key_ok(g0->vartype, g0->varcollid)) {
		return false;
	}
	m->p[P_XATT] = g0->varattno;
	m->p[P_XTYPE] = g0->vartype;
	bool have_eq = false, have_cmp = false;
	ListCell *lc;
	foreach (lc, quals) {
		const char *op;
		Var *l, *r;
		Oid coll;
		if (!binop_vars((Node *)lfirst(lc), va, vb, &op, &l, &r, &coll)) {
			return false;
		}
		if (strcmp(op, "=") == 0 && !have_eq && l->varattno != m->p[P_XATT] && key_ok(l->vartype, coll)) {
			m->p[P_KATT] = l->varattno;
			m->p[P_KTYPE] = l->vartype;
			have_eq = true;
		} else if (!have_cmp && l->varattno == m->p[P_XATT] &&
		           (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) &&
		           (!str_type(l->vartype) || (OidIsValid(coll) && get_collation_isdeterministic(coll)))) {
			bool lt = op[0] == '<';
			m->p[P_XCOLL] = str_type(l->vartype) && !pg_newlocale_from_collation(coll)->collate_is_c ? coll : 0;
			m->p[P_MODE] = op[1] == '=' ? 2 : 1;
			int smaller = lt ? l->varno : r->varno;
			m->p[P_COL0_SMALLER] = smaller == va;
			have_cmp = true;
		} else if (!have_cmp && l->varattno == m->p[P_XATT] && strcmp(op, "<>") == 0 && key_ok(l->vartype, coll)) {
			m->p[P_MODE] = 3;
			have_cmp = true;
		} else {
			return false;
		}
	}
	if (!have_eq) {
		return false;
	}
	if (!att_not_null(ra->relid, (AttrNumber)m->p[P_XATT])) {
		return false;
	}
	return match_agg(root, m, va, vb, g0, g1, group_tlist(parse, output_rel));
}

typedef struct HavingRefs {
	Aggref *agg;
	Var *g0, *g1;
	bool ok;
} HavingRefs;

static bool having_refs_walker(Node *n, HavingRefs *h) {
	if (!n) {
		return false;
	}
	if (IsA(n, Aggref)) {
		if (!equal(n, h->agg)) {
			h->ok = false;
		}
		return false;
	}
	if (IsA(n, Var)) {
		Var *v = (Var *)n;
		if (!(v->varlevelsup == 0 && (equal(v, h->g0) || equal(v, h->g1)))) {
			h->ok = false;
		}
		return false;
	}
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Param)) {
		h->ok = false;
		return false;
	}
	return expression_tree_walker(n, having_refs_walker, (void *)h);
}

static bool count_all(TesseraMatch *m, bool cross, Aggref *a) {
	if (list_length(a->args) != 1) {
		return false;
	}
	Node *e = (Node *)linitial_node(TargetEntry, a->args)->expr;
	if (IsA(e, Const)) {
		return !((Const *)e)->constisnull;
	}
	Var *v = as_var(e);
	if (!v || v->varlevelsup != 0) {
		return false;
	}
	Oid rel = cross && v->varno != m->va ? (Oid)m->p[P_RELID2] : (Oid)m->p[P_RELID];
	return att_not_null(rel, v->varattno);
}

static bool plain_condition(Node *q) {
	if (IsA(q, BoolExpr)) {
		ListCell *lc;
		foreach (lc, ((BoolExpr *)q)->args) {
			if (!plain_condition((Node *)lfirst(lc))) {
				return false;
			}
		}
		return true;
	}
	if (!IsA(q, OpExpr) || list_length(((OpExpr *)q)->args) != 2) {
		return false;
	}
	const char *op = catalog_opname(((OpExpr *)q)->opno);
	if (!op || (strcmp(op, "<") != 0 && strcmp(op, "<=") != 0 && strcmp(op, ">") != 0 && strcmp(op, ">=") != 0 &&
	            strcmp(op, "=") != 0 && strcmp(op, "<>") != 0)) {
		return false;
	}
	ListCell *lc;
	foreach (lc, ((OpExpr *)q)->args) {
		Node *e = (Node *)lfirst(lc);
		if (!as_var(e) && !IsA(e, Aggref) && !IsA(e, Const)) {
			return false;
		}
	}
	return true;
}

static void having_prune(List *having, TesseraMatch *m) {
	ListCell *lc;
	foreach (lc, having) {
		if (!plain_condition((Node *)lfirst(lc))) {
			return;
		}
	}
	foreach (lc, having) {
		Node *q = (Node *)lfirst(lc);
		if (!IsA(q, OpExpr) || list_length(((OpExpr *)q)->args) != 2) {
			continue;
		}
		OpExpr *o = (OpExpr *)q;
		const char *op = catalog_opname(o->opno);
		Node *l = linitial(o->args), *r = lsecond(o->args);
		bool flip = false;
		if (!equal(l, m->agg)) {
			Node *t = l;
			l = r;
			r = t;
			flip = true;
		}
		if (!op || !equal(l, m->agg) || !IsA(r, Const) || ((Const *)r)->constisnull) {
			continue;
		}
		const bool ge = strcmp(op, flip ? "<=" : ">=") == 0, gt = strcmp(op, flip ? "<" : ">") == 0;
		Const *c = (Const *)r;
		int64 lo;
		if (int_type(c->consttype)) {
			const int64 v = c->consttype == INT8OID ? DatumGetInt64(c->constvalue)
			                : c->consttype == INT4OID ? DatumGetInt32(c->constvalue)
			                                          : DatumGetInt16(c->constvalue);
			if (int_type(exprType((Node *)m->agg))) {
				lo = gt && v < PG_INT64_MAX ? v + 1 : v;
			} else if (v < ((int64)1 << 53)) {
				lo = v;
			} else {
				continue;
			}
		} else if (c->consttype == FLOAT8OID || c->consttype == FLOAT4OID) {
			const double v = c->consttype == FLOAT8OID ? DatumGetFloat8(c->constvalue) : DatumGetFloat4(c->constvalue);
			if (!(v >= 1 && v < 0x1p53)) {
				continue;
			}
			lo = (int64)floor(v);
		} else {
			continue;
		}
		if ((ge || gt) && lo > m->p[P_MINVAL]) {
			m->p[P_MINVAL] = lo;
		}
	}
}

static bool lower_bound_of(Node *q, Node *agg) {
	if (!IsA(q, OpExpr) || list_length(((OpExpr *)q)->args) != 2) {
		return false;
	}
	const char *op = catalog_opname(((OpExpr *)q)->opno);
	Node *l = linitial(((OpExpr *)q)->args), *r = lsecond(((OpExpr *)q)->args);
	const bool flip = !equal(l, agg);
	if (flip) {
		Node *t = l;
		l = r;
		r = t;
	}
	return op && equal(l, agg) && IsA(r, Const) && !((Const *)r)->constisnull &&
	       (strcmp(op, flip ? "<=" : ">=") == 0 || strcmp(op, flip ? "<" : ">") == 0);
}

static bool plain_targets(Query *parse) {
	ListCell *lc;
	foreach (lc, parse->targetList) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		if (!as_var((Node *)te->expr) && !IsA(te->expr, Aggref) && !IsA(te->expr, Const) && !IsA(te->expr, WindowFunc) &&
		    !(te->ressortgroupref && get_sortgroupref_clause_noerr(te->ressortgroupref, parse->groupClause))) {
			return false;
		}
	}
	return true;
}

static int64 rank_limit(Query *parse, Node *agg, List **partition) {
	if (!parse->hasWindowFuncs || !agg || list_length(parse->windowClause) != 1 || !plain_targets(parse)) {
		return -1;
	}
	WindowClause *wc = linitial_node(WindowClause, parse->windowClause);
	WindowFuncLists *wl = find_window_functions((Node *)parse->targetList, wc->winref);
	if (wl->numWindowFuncs != 1 || wc->orderClause == NIL || wc->partitionClause == NIL) {
		return -1;
	}
	WindowFunc *wf = linitial_node(WindowFunc, wl->windowFuncs[wc->winref]);
	if ((wf->winfnoid != 3100 && wf->winfnoid != 3101) || wf->aggfilter || wf->runCondition == NIL) {
		return -1;
	}
	ListCell *lc;
	foreach (lc, (List *)parse->havingQual) {
		if (!lower_bound_of((Node *)lfirst(lc), agg)) {
			return -1;
		}
	}
	SortGroupClause *sc = linitial_node(SortGroupClause, wc->orderClause);
	Oid opfamily, opcintype;
	CompareType cmp;
	if (!equal(get_sortgroupclause_expr(sc, parse->targetList), agg) ||
	    !get_ordering_op_properties(sc->sortop, &opfamily, &opcintype, &cmp) || cmp != COMPARE_GT) {
		return -1;
	}
	int64 k = -1;
	foreach (lc, wf->runCondition) {
		WindowFuncRunCondition *rc = lfirst_node(WindowFuncRunCondition, lc);
		if (!IsA(rc->arg, Const) || ((Const *)rc->arg)->constisnull ||
		    (((Const *)rc->arg)->consttype != INT8OID && ((Const *)rc->arg)->consttype != INT4OID)) {
			continue;
		}
		const int64 c = ((Const *)rc->arg)->consttype == INT8OID ? DatumGetInt64(((Const *)rc->arg)->constvalue)
		                                                         : (int64)DatumGetInt32(((Const *)rc->arg)->constvalue);
		List *interp = get_op_index_interpretation(rc->opno);
		if (interp == NIL) {
			continue;
		}
		CompareType t = ((OpIndexInterpretation *)linitial(interp))->cmptype;
		if (!rc->wfunc_left) {
			t = t == COMPARE_GE ? COMPARE_LE : t == COMPARE_GT ? COMPARE_LT : COMPARE_INVALID;
		}
		const int64 lim = t == COMPARE_LE ? c : t == COMPARE_LT ? c - 1 : -1;
		if (lim >= 1 && (k < 0 || lim < k)) {
			k = lim;
		}
	}
	if (k > 0) {
		*partition = wc->partitionClause;
	}
	return k;
}

static void limit_prune(Query *parse, TesseraMatch *m) {
	ListCell *lh;
	if (!plain_targets(parse)) {
		return;
	}
	foreach (lh, (List *)parse->havingQual) {
		if (!lower_bound_of((Node *)lfirst(lh), (Node *)m->agg)) {
			return;
		}
	}
	if (m->agg && parse->sortClause && parse->limitCount && IsA(parse->limitCount, Const) &&
	    !((Const *)parse->limitCount)->constisnull && ((Const *)parse->limitCount)->consttype == INT8OID) {
		SortGroupClause *sc = linitial_node(SortGroupClause, parse->sortClause);
		Node *se = (Node *)get_sortgroupclause_expr(sc, parse->targetList);
		Oid opfamily, opcintype;
		CompareType cmp;
		const int64 n = DatumGetInt64(((Const *)parse->limitCount)->constvalue);
		int64 off = 0;
		bool ok = equal(se, m->agg) && get_ordering_op_properties(sc->sortop, &opfamily, &opcintype, &cmp) &&
		          cmp == COMPARE_GT;
		if (parse->limitOffset) {
			Const *oc = IsA(parse->limitOffset, Const) ? (Const *)parse->limitOffset : NULL;
			ok = ok && oc && !oc->constisnull && oc->consttype == INT8OID;
			off = ok ? DatumGetInt64(oc->constvalue) : 0;
		}
		if (ok && n > 0 && off >= 0 && n <= PG_INT64_MAX - off) {
			m->p[P_TOPK] = n + off;
		}
	}
}

static bool match_agg(PlannerInfo *root, TesseraMatch *m, int va, int vb, Var *g0, Var *g1, List *tl) {
	Query *parse = root->parse;
	const bool cross = m->p[P_CROSS] != 0;
	ListCell *lc;
	foreach (lc, tl) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		if (te->resjunk) {
			continue;
		}
		if (as_var((Node *)te->expr) && (equal(as_var((Node *)te->expr), g0) || equal(as_var((Node *)te->expr), g1))) {
			continue;
		}
		if (!IsA(te->expr, Aggref) || m->agg) {
			return false;
		}
		Aggref *a = (Aggref *)te->expr;
		if (a->aggorder || a->aggfilter || a->aggkind != AGGKIND_NORMAL || a->agglevelsup != 0) {
			return false;
		}
		char *fn = get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE ? get_func_name(a->aggfnoid) : NULL;
		if (a->aggdistinct) {
			Var *dv = list_length(a->args) == 1 ? as_var((Node *)linitial_node(TargetEntry, a->args)->expr) : NULL;
			if (!fn || strcmp(fn, "count") != 0 || !dv || list_length(a->aggdistinct) != 1 ||
			    !((dv->varno == va && dv->varattno == m->p[P_KATT]) ||
			      (dv->varno == vb && dv->varattno == (cross ? m->p[P_KATT2] : m->p[P_KATT])))) {
				return false;
			}
			m->p[P_VATT] = 0;
			m->p[P_DISTINCT] = 1;
		} else if (fn && strcmp(fn, "count") == 0 && (a->aggstar || count_all(m, cross, a))) {
			m->p[P_VATT] = 0;
		} else if (fn && strcmp(fn, "sum") == 0 && list_length(a->args) == 1) {
			const char *op;
			Var *l, *r;
			Oid coll;
			Node *e = (Node *)linitial_node(TargetEntry, a->args)->expr;
			if (cross) {
				if (!binop_cross(e, va, vb, &op, &l, &r, &coll) || strcmp(op, "*") != 0 ||
				    !(int_type(l->vartype) || l->vartype == FLOAT8OID || l->vartype == FLOAT4OID) ||
				    !(int_type(r->vartype) || r->vartype == FLOAT8OID || r->vartype == FLOAT4OID) ||
				    !att_not_null((Oid)m->p[P_RELID], l->varattno) || !att_not_null((Oid)m->p[P_RELID2], r->varattno)) {
					return false;
				}
				m->p[P_VATT2] = r->varattno;
				m->p[P_VTYPE2] = r->vartype;
			} else if (!binop_vars(e, va, vb, &op, &l, &r, &coll) || strcmp(op, "*") != 0 ||
			           l->varattno == m->p[P_KATT] ||
			           !(int_type(l->vartype) || l->vartype == FLOAT8OID || l->vartype == FLOAT4OID) ||
			           !att_not_null((Oid)m->p[P_RELID], l->varattno)) {
				return false;
			}
			m->p[P_VATT] = l->varattno;
			m->p[P_VTYPE] = l->vartype;
		} else {
			return false;
		}
		if (a->aggtype != INT8OID && a->aggtype != FLOAT8OID && a->aggtype != NUMERICOID) {
			return false;
		}
		m->p[P_AGGTYPE] = a->aggtype;
		m->agg = a;
	}
	foreach (lc, tl) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		if (!te->resjunk) {
			continue;
		}
		Var *v = as_var((Node *)te->expr);
		if (v && (equal(v, g0) || equal(v, g1))) {
			continue;
		}
		if (!m->agg || !IsA(te->expr, Aggref) || !equal(te->expr, m->agg)) {
			return false;
		}
	}
	List *having = (List *)parse->havingQual;
	if (having) {
		HavingRefs hr = {m->agg, g0, g1, true};
		if (!m->agg || having_refs_walker((Node *)having, &hr) || !hr.ok) {
			return false;
		}
		having_prune(having, m);
	}
	if (!parse->hasWindowFuncs) {
		limit_prune(parse, m);
	} else if (m->agg) {
		List *part = NIL;
		const int64 k = rank_limit(parse, (Node *)m->agg, &part);
		if (k > 0 && list_length(part) == 1) {
			Var *pv = as_var((Node *)get_sortgroupclause_expr(linitial_node(SortGroupClause, part), parse->targetList));
			if (pv && (equal(pv, g0) || equal(pv, g1))) {
				const bool first = pv->varno == va;
				m->p[P_GROUP_TOPK] = k;
				m->p[P_GROUP_OF] = m->p[P_CROSS] ? (first ? 3 : 4)
				                   : m->p[P_MODE] == 0 || m->p[P_MODE] == 3 ? 0
				                   : first == (m->p[P_COL0_SMALLER] != 0)   ? 1
				                                                            : 2;
			}
		}
	}
	return true;
}

static bool match_cross(PlannerInfo *root, TesseraMatch *m, Var *g0, Var *g1, List *tl) {
	Query *parse = root->parse;
	const int vx = g0->varno, vy = g1->varno;
	RangeTblEntry *rx = rt_fetch(vx, parse->rtable), *ry = rt_fetch(vy, parse->rtable);
	if (!key_ok(g0->vartype, g0->varcollid) || !key_ok(g1->vartype, g1->varcollid)) {
		return false;
	}
	m->va = vx;
	m->vb = vy;
	m->p[P_CROSS] = 1;
	m->p[P_RELID] = rx->relid;
	m->p[P_XATT] = g0->varattno;
	m->p[P_XTYPE] = g0->vartype;
	m->p[P_RELID2] = ry->relid;
	m->p[P_XATT2] = g1->varattno;
	m->p[P_XTYPE2] = g1->vartype;
	int ja, jb;
	List *quals;
	if (!join_inputs(parse, &ja, &jb, &quals)) {
		return false;
	}
	const char *op;
	Var *kx, *ky;
	Oid coll;
	if (list_length(quals) != 1 || !binop_cross((Node *)linitial(quals), vx, vy, &op, &kx, &ky, &coll) ||
	    strcmp(op, "=") != 0 || !key_ok(kx->vartype, coll) || !key_ok(ky->vartype, coll) ||
	    !keys_compatible(kx->vartype, ky->vartype)) {
		return false;
	}
	m->p[P_KATT] = kx->varattno;
	m->p[P_KTYPE] = kx->vartype;
	m->p[P_KATT2] = ky->varattno;
	m->p[P_KTYPE2] = ky->vartype;
	if (!att_not_null(rx->relid, g0->varattno) || !att_not_null(ry->relid, g1->varattno)) {
		return false;
	}
	return match_agg(root, m, vx, vy, g0, g1, tl);
}

static List *params_list(const int64 *p) {
	List *l = NIL;
	for (int i = 0; i < P_NPARAM; i++) {
		l = lappend(l, makeConst(INT8OID, -1, InvalidOid, 8, Int64GetDatum(p[i]), false, true));
	}
	return l;
}

static Plan *tessera_plan_path(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist,
                               List *clauses, List *custom_plans);
static Node *tessera_create_state(CustomScan *cscan);

static const CustomPathMethods tessera_path_methods = {
    .CustomName = "Tessera",
    .PlanCustomPath = tessera_plan_path,
};
static const CustomScanMethods tessera_scan_methods = {
    .CustomName = "Tessera",
    .CreateCustomScanState = tessera_create_state,
};

static double rel_rows(PlannerInfo *root, int rti) {
	RelOptInfo *rel = find_base_rel(root, rti);
	return rel->tuples > 0 ? rel->tuples : -1;
}

static double key_distinct(PlannerInfo *root, int rti, AttrNumber att, Oid type) {
	VariableStatData vd;
	bool isdefault;
	examine_variable(root, (Node *)makeVar(rti, att, type, -1, InvalidOid, 0), 0, &vd);
	const double n = get_variable_numdistinct(&vd, &isdefault);
	ReleaseVariableStats(vd);
	return isdefault || n <= 0 ? -1 : n;
}

static bool join_expands(PlannerInfo *root, const TesseraMatch *m) {
	const bool cross = m->p[P_CROSS] != 0;
	const double theta = cross ? tessera_min_expansion_cross : tessera_min_expansion;
	const double na = rel_rows(root, m->va), da = key_distinct(root, m->va, m->p[P_KATT], m->p[P_KTYPE]);
	double e = -1;
	if (na > 0 && da > 0 && !cross) {
		e = na / da;
	} else if (na > 0 && da > 0) {
		const double nb = rel_rows(root, m->vb), db = key_distinct(root, m->vb, m->p[P_KATT2], m->p[P_KTYPE2]);
		if (nb > 0 && db > 0) {
			e = na * nb / (Max(da, db) * (na + nb));
		}
	}
	const bool run = theta <= 0 || e < 0 || e >= theta;
	if (tessera_verbose) {
		elog(LOG, "tessera: %.3g joined rows per input row (estimate), limit %.3g: %s", e, theta,
		     run ? "Tessera" : "PostgreSQL's plan");
	}
	return run;
}

#define TESSERA_MAXCOLS 8

typedef struct TesseraSide {
	int rti;
	Oid relid;
	List *quals;
	int nk, ng;
	AttrNumber katt[TESSERA_MAXCOLS], gatt[TESSERA_MAXCOLS];
	Oid ktype[TESSERA_MAXCOLS], gtype[TESSERA_MAXCOLS];
	bool gnull[TESSERA_MAXCOLS];
	Node *gexpr[TESSERA_MAXCOLS];
	Node *kexpr[TESSERA_MAXCOLS];
	AttrNumber vatt;
	Oid vtype;
	int32 vtypmod;
	bool vcast;
	int child;
	AttrNumber kpos[TESSERA_MAXCOLS], gpos[TESSERA_MAXCOLS], vpos;
	bool inh;
} TesseraSide;

typedef struct TesseraGeneral {
	TesseraSide side[2];
	int nfilter;
	int fx[TESSERA_MAXCOLS], fy[TESSERA_MAXCOLS], fop[TESSERA_MAXCOLS];
	Oid fcoll[TESSERA_MAXCOLS];
	bool has_v, distinct;
	Oid aggtype;
	int64 minval, topk;
	int64 group_topk;
	int group_side;
	int nmany;
	int many_nf[TESSERA_MAXSUMS][2];
	AttrNumber many_att[TESSERA_MAXSUMS][2][TESSERA_MAXFACTORS];
	bool many_null;
	bool many_count[TESSERA_MAXSUMS];
	int many_bits[TESSERA_MAXSUMS];
	bool many_avg[TESSERA_MAXSUMS];
	bool null_ok;
	List *many_aggs;
	Aggref *agg;
	bool two;
	Aggref *agg2, *agg3;
	Oid aggtype2, avgtype;
	bool symmetric;
} TesseraGeneral;

static bool is_float8_mul(Node *e) {
	return IsA(e, OpExpr) && list_length(((OpExpr *)e)->args) == 2 && catalog_opname(((OpExpr *)e)->opno) &&
	       strcmp(catalog_opname(((OpExpr *)e)->opno), "*") == 0;
}

static int many_chain(TesseraGeneral *g, int m, Node *e, int rx, int ry) {
	if (is_float8_mul(e)) {
		Node *l = (Node *)linitial(((OpExpr *)e)->args), *r = (Node *)lsecond(((OpExpr *)e)->args);
		if (is_float8_mul(r)) {
			Node *t = l;
			l = r;
			r = t;
		}
		if (is_float8_mul(r)) {
			return -1;
		}
		const int a = many_chain(g, m, l, rx, ry), b = a < 0 ? -1 : many_chain(g, m, r, rx, ry);
		return a == b ? a : -1;
	}
	if (!IsA(e, Var)) {
		return -1;
	}
	Var *v = (Var *)e;
	if (v->varlevelsup != 0 || v->varattno <= 0 || v->vartype != FLOAT8OID || (v->varno != rx && v->varno != ry)) {
		return -1;
	}
	const int s = v->varno == rx ? 0 : 1;
	if (g->many_nf[m][s] == TESSERA_MAXFACTORS) {
		return -1;
	}
	if (!att_not_null(g->side[s].relid, v->varattno)) {
		if (!g->null_ok) {
			return -1;
		}
		g->many_null = true;
	}
	g->many_att[m][s][g->many_nf[m][s]++] = v->varattno;
	return s;
}

static bool many_factors(TesseraGeneral *g, int m, Node *e, int rx, int ry) {
	if (many_chain(g, m, e, rx, ry) >= 0) {
		return true;
	}
	g->many_nf[m][0] = g->many_nf[m][1] = 0;
	if (!is_float8_mul(e)) {
		return false;
	}
	const int a = many_chain(g, m, (Node *)linitial(((OpExpr *)e)->args), rx, ry),
	          b = a < 0 ? -1 : many_chain(g, m, (Node *)lsecond(((OpExpr *)e)->args), rx, ry);
	return a >= 0 && b >= 0 && a != b;
}

static bool unsafe_qual_walker(Node *n, void *ctx) {
	if (!n) {
		return false;
	}
	if (IsA(n, Param) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Aggref) || IsA(n, WindowFunc) ||
	    IsA(n, GroupingFunc)) {
		return true;
	}
	return expression_tree_walker(n, unsafe_qual_walker, ctx);
}

static int general_group(const TesseraSide *s, const Var *v) {
	for (int i = 0; i < s->ng; i++) {
		if (!s->gexpr[i] && s->rti == v->varno && s->gatt[i] == v->varattno) {
			return i;
		}
	}
	return -1;
}

static int general_group_node(const TesseraSide *s, Node *n) {
	Var *v = as_var(n);
	if (v) {
		return general_group(s, v);
	}
	for (int i = 0; i < s->ng; i++) {
		if (s->gexpr[i] && equal(s->gexpr[i], n)) {
			return i;
		}
	}
	return -1;
}

static bool safe_group_expr(Node *n) {
	if (!n) {
		return false;
	}
	if (IsA(n, Var)) {
		return ((Var *)n)->varlevelsup == 0 && ((Var *)n)->varattno > 0;
	}
	if (IsA(n, Const)) {
		return !((Const *)n)->constisnull;
	}
	if (IsA(n, RelabelType)) {
		return safe_group_expr((Node *)((RelabelType *)n)->arg);
	}
	if (IsA(n, CoerceViaIO)) {
		CoerceViaIO *c = (CoerceViaIO *)n;
		return int_type(exprType((Node *)c->arg)) && str_type(c->resulttype) && safe_group_expr((Node *)c->arg);
	}
	Oid fn;
	List *args;
	if (IsA(n, OpExpr)) {
		set_opfuncid((OpExpr *)n);
		fn = ((OpExpr *)n)->opfuncid;
		args = ((OpExpr *)n)->args;
	} else if (IsA(n, FuncExpr)) {
		fn = ((FuncExpr *)n)->funcid;
		args = ((FuncExpr *)n)->args;
	} else {
		return false;
	}
	if (get_func_namespace(fn) != PG_CATALOG_NAMESPACE) {
		return false;
	}
	const char *name = get_func_name(fn);
	ListCell *lc;
	foreach (lc, args) {
		if (!safe_group_expr((Node *)lfirst(lc))) {
			return false;
		}
	}
	const int na = list_length(args);
	Node *last = na ? (Node *)llast(args) : NULL;
	const bool last_const = last && IsA(last, Const);
	int64 lv = last_const && int_type(((Const *)last)->consttype) ? datum_int(((Const *)last)->constvalue, ((Const *)last)->consttype) : 0;
	if (!strcmp(name, "int2mod") || !strcmp(name, "int4mod") || !strcmp(name, "int8mod")) {
		return na == 2 && last_const && lv != 0;
	}
	if (!strcmp(name, "int2div") || !strcmp(name, "int4div") || !strcmp(name, "int8div")) {
		return na == 2 && last_const && lv != 0 && lv != -1;
	}
	if (!strcmp(name, "textcat") || !strcmp(name, "lower") || !strcmp(name, "upper")) {
		return true;
	}
	if (!strcmp(name, "substr") || !strcmp(name, "substring")) {
		return (na == 2 || na == 3) && IsA(lsecond(args), Const) && int_type(exprType(lsecond(args))) &&
		       (na == 2 || (last_const && lv >= 0)) && str_type(exprType(linitial(args)));
	}
	if (!strcmp(name, "date_trunc") && na == 2 && IsA(linitial(args), Const)) {
		static const char *units[] = {"microseconds", "milliseconds", "second", "minute", "hour", "day", "week",
		                              "month", "quarter", "year", "decade", "century", "millennium"};
		const Oid t = exprType(lsecond(args));
		char *u = TextDatumGetCString(((Const *)linitial(args))->constvalue);
		for (int i = 0; i < (int)lengthof(units); i++) {
			if (pg_strcasecmp(u, units[i]) == 0) {
				return t == TIMESTAMPOID || t == TIMESTAMPTZOID;
			}
		}
	}
	return false;
}

typedef struct GeneralRefs {
	TesseraGeneral *g;
	bool ok;
} GeneralRefs;

static bool general_refs_walker(Node *n, GeneralRefs *h) {
	if (!n) {
		return false;
	}
	if (general_group_node(&h->g->side[0], n) >= 0 || general_group_node(&h->g->side[1], n) >= 0) {
		return false;
	}
	if (IsA(n, Aggref)) {
		if (!(h->g->agg && equal(n, h->g->agg)) && !(h->g->agg2 && equal(n, h->g->agg2)) &&
		    !(h->g->agg3 && equal(n, h->g->agg3)) && !list_member(h->g->many_aggs, n)) {
			h->ok = false;
		}
		return false;
	}
	if (IsA(n, Var)) {
		Var *v = (Var *)n;
		if (v->varlevelsup != 0 || (general_group(&h->g->side[0], v) < 0 && general_group(&h->g->side[1], v) < 0)) {
			h->ok = false;
		}
		return false;
	}
	if (IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Param)) {
		h->ok = false;
		return false;
	}
	return expression_tree_walker(n, general_refs_walker, (void *)h);
}

typedef struct OutputItems {
	const TesseraGeneral *g;
	List *items;
	bool ok;
} OutputItems;

static bool output_items_walker(Node *n, OutputItems *o) {
	if (!n) {
		return false;
	}
	if (general_group_node(&o->g->side[0], n) >= 0 || general_group_node(&o->g->side[1], n) >= 0 || IsA(n, Aggref)) {
		if (!list_member(o->items, n)) {
			o->items = lappend(o->items, n);
		}
		return false;
	}
	if (IsA(n, Var) || IsA(n, SubLink) || IsA(n, SubPlan) || IsA(n, Param) || IsA(n, WindowFunc) || IsA(n, GroupingFunc)) {
		o->ok = false;
		return false;
	}
	return expression_tree_walker(n, output_items_walker, (void *)o);
}

static bool weight_type(Oid t) {
	return int_type(t) || t == FLOAT8OID || t == FLOAT4OID || t == NUMERICOID;
}

static bool numeric_typmod(int32 typmod, int *prec, int *scale) {
	if (typmod < (int32)VARHDRSZ) {
		return false;
	}
	*prec = ((typmod - VARHDRSZ) >> 16) & 0xffff;
	*scale = (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
	return true;
}

static int weight_scale(const TesseraSide *d) {
	int p, sc;
	return d->vatt && d->vtype == NUMERICOID && !d->vcast && numeric_typmod(d->vtypmod, &p, &sc) ? sc : 0;
}

static bool same_computed(const TesseraSide *x, const TesseraSide *y) {
	for (int i = 0; i < x->ng + x->nk; i++) {
		Node *a = i < x->ng ? x->gexpr[i] : x->kexpr[i - x->ng], *b = i < x->ng ? y->gexpr[i] : y->kexpr[i - x->ng];
		if ((a == NULL) != (b == NULL)) {
			return false;
		}
		if (b) {
			Node *c = (Node *)copyObject(b);
			ChangeVarNodes(c, y->rti, x->rti, 0);
			if (!equal(c, a)) {
				return false;
			}
		}
	}
	return true;
}

static int general_aggregate(TesseraGeneral *g, Aggref *a, int rx, int ry) {
	if (a->aggorder || a->aggfilter || a->aggkind != AGGKIND_NORMAL || a->agglevelsup != 0 ||
	    (a->aggtype != INT8OID && a->aggtype != FLOAT8OID && a->aggtype != NUMERICOID)) {
		return 0;
	}
	char *fn = get_func_namespace(a->aggfnoid) == PG_CATALOG_NAMESPACE ? get_func_name(a->aggfnoid) : NULL;
	Node *arg = list_length(a->args) == 1 ? (Node *)linitial_node(TargetEntry, a->args)->expr : NULL;
	Var *av = arg ? as_var(arg) : NULL;
	if (a->aggdistinct) {
		if (!fn || strcmp(fn, "count") != 0 || !av || list_length(a->aggdistinct) != 1 || g->side[0].nk != 1 ||
		    !((av->varno == rx && av->varattno == g->side[0].katt[0]) ||
		      (av->varno == ry && av->varattno == g->side[1].katt[0]))) {
			return 0;
		}
		g->distinct = true;
		return 1;
	}
	if (fn && strcmp(fn, "count") == 0 &&
	    (a->aggstar || (arg && IsA(arg, Const) && !((Const *)arg)->constisnull) ||
	     (av && (av->varno == rx || av->varno == ry) && att_not_null(g->side[av->varno == rx ? 0 : 1].relid, av->varattno)))) {
		return 1;
	}
	if (!fn || (strcmp(fn, "sum") != 0 && strcmp(fn, "avg") != 0) || !arg) {
		return 0;
	}
	Var *l, *r;
	if (av) {
		if (av->varno != rx && av->varno != ry) {
			return 0;
		}
		TesseraSide *s = &g->side[av->varno == rx ? 0 : 1];
		s->vatt = av->varattno;
		s->vtype = av->vartype;
		s->vtypmod = av->vartypmod;
	} else if (IsA(arg, OpExpr) && list_length(((OpExpr *)arg)->args) == 2 && catalog_opname(((OpExpr *)arg)->opno) &&
	           strcmp(catalog_opname(((OpExpr *)arg)->opno), "*") == 0) {
		bool c0, c1;
		Var *v0 = cast_var((Node *)linitial(((OpExpr *)arg)->args), &c0), *v1 = cast_var((Node *)lsecond(((OpExpr *)arg)->args), &c1);
		if (!v0 || !v1 || !((v0->varno == rx && v1->varno == ry) || (v0->varno == ry && v1->varno == rx))) {
			return 0;
		}
		const bool first_x = v0->varno == rx;
		l = first_x ? v0 : v1;
		r = first_x ? v1 : v0;
		g->side[0].vatt = l->varattno;
		g->side[0].vtype = l->vartype;
		g->side[0].vtypmod = l->vartypmod;
		g->side[0].vcast = first_x ? c0 : c1;
		g->side[1].vatt = r->varattno;
		g->side[1].vtype = r->vartype;
		g->side[1].vtypmod = r->vartypmod;
		g->side[1].vcast = first_x ? c1 : c0;
		if (l->vartype == FLOAT4OID && r->vartype == FLOAT4OID && !g->side[0].vcast && !g->side[1].vcast) {
			return 0;
		}
	} else {
		return 0;
	}
	int digits = 0;
	for (int s = 0; s < 2; s++) {
		const TesseraSide *d = &g->side[s];
		int p, sc;
		if (d->vatt && (!weight_type(d->vtype) || !att_not_null(d->relid, d->vatt))) {
			return 0;
		}
		if (d->vatt && d->vtype == NUMERICOID && !d->vcast) {
			if (!numeric_typmod(d->vtypmod, &p, &sc) || sc < 0 || sc > p) {
				return 0;
			}
			digits += p;
		}
	}
	if (digits > 18) {
		return 0;
	}
	g->has_v = true;
	return strcmp(fn, "sum") == 0 ? 2 : 3;
}

static bool general_input_ok(RangeTblEntry *rte) {
	if (rte->rtekind != RTE_RELATION || rte->tablesample ||
	    (rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_PARTITIONED_TABLE)) {
		return false;
	}
	if (!rte->inh) {
		return rte->relkind == RELKIND_RELATION;
	}
	ListCell *lc;
	List *rels = find_all_inheritors(rte->relid, NoLock, NULL);
	foreach (lc, rels) {
		const char k = get_rel_relkind(lfirst_oid(lc));
		if (k != RELKIND_RELATION && k != RELKIND_PARTITIONED_TABLE) {
			return false;
		}
	}
	return true;
}

static bool child_input_ok(PlannerInfo *root, int rti) {
	RelOptInfo *rel = rti < root->simple_rel_array_size ? root->simple_rel_array[rti] : NULL;
	RangeTblEntry *rte = rt_fetch(rti, root->parse->rtable);
	return rel && rel->reloptkind == RELOPT_BASEREL && rel->cheapest_total_path &&
	       !rel->cheapest_total_path->param_info && bms_is_empty(rel->lateral_relids) &&
	       (rte->rtekind == RTE_RELATION || rte->rtekind == RTE_SUBQUERY || rte->rtekind == RTE_CTE ||
	        rte->rtekind == RTE_FUNCTION || rte->rtekind == RTE_VALUES);
}

static bool group_tuples(const TesseraSide *d) {
	return d->ng > 1 || d->gnull[0];
}

static bool group_dict(const TesseraSide *d) {
	return group_tuples(d) || bytes_type(d->gtype[0]);
}

static int many_pair_op(const TesseraGeneral *g) {
	const TesseraSide *x = &g->side[0], *y = &g->side[1];
	if (g->nfilter != 1 || x->ng != 1 || y->ng != 1 || group_dict(x) || group_dict(y)) {
		return 0;
	}
	return g->fop[0] == 0 ? 1 : g->fop[0] == 1 ? 2 : g->fop[0] == 2 ? 3 : g->fop[0] == 3 ? 4 : 5;
}

static bool match_general(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *output_rel, TesseraGeneral *g,
                          bool null_ok) {
	Query *parse = root->parse;
	int va, vb;
	List *quals;
	if (stage != UPPERREL_GROUP_AGG || parse->commandType != CMD_SELECT || parse->setOperations || parse->groupingSets ||
	    parse->hasTargetSRFs || parse->distinctClause || parse->hasSubLinks || list_length(parse->groupClause) < 2 ||
	    !join_inputs(parse, &va, &vb, &quals)) {
		return false;
	}
	RangeTblEntry *ra = rt_fetch(va, parse->rtable), *rb = rt_fetch(vb, parse->rtable);
	const bool child_a = !general_input_ok(ra), child_b = !general_input_ok(rb);
	if ((child_a && !child_input_ok(root, va)) || (child_b && !child_input_ok(root, vb))) {
		return false;
	}
	memset(g, 0, sizeof *g);
	ListCell *lc;
	int sx = -1;
	foreach (lc, parse->groupClause) {
		Node *e = (Node *)get_sortgroupclause_expr(lfirst_node(SortGroupClause, lc), parse->targetList);
		Var *v = as_var(e);
		Oid c = exprCollation(e);
		Relids rels = v ? NULL : pull_varnos(root, e);
		int rel = v ? (int)v->varno : bms_num_members(rels) == 1 ? bms_singleton_member(rels) : -1;
		if ((rel != va && rel != vb) || !(key_ok(exprType(e), c) || text_key_ok(exprType(e), exprTypmod(e))) ||
		    (!v && (!safe_group_expr(e) || contain_volatile_functions(e) || (rel == va ? child_a : child_b))) ||
		    (str_type(exprType(e)) && !(OidIsValid(c) && get_collation_isdeterministic(c)))) {
			return false;
		}
		if (sx < 0) {
			sx = rel;
			g->side[0].rti = rel;
			g->side[1].rti = rel == va ? vb : va;
		}
		TesseraSide *s = &g->side[rel == sx ? 0 : 1];
		if (general_group_node(s, e) >= 0) {
			continue;
		}
		if (s->ng == TESSERA_MAXCOLS) {
			return false;
		}
		s->gatt[s->ng] = v ? v->varattno : 0;
		s->gtype[s->ng] = exprType(e);
		s->gexpr[s->ng] = v ? NULL : (Node *)copyObject(e);
		s->gnull[s->ng] = !v;
		s->ng++;
	}
	if (g->side[0].ng == 0 || g->side[1].ng == 0) {
		return false;
	}
	g->side[0].relid = rt_fetch(g->side[0].rti, parse->rtable)->relid;
	g->side[1].relid = rt_fetch(g->side[1].rti, parse->rtable)->relid;
	for (int s = 0; s < 2; s++) {
		RangeTblEntry *rte = rt_fetch(g->side[s].rti, parse->rtable);
		g->side[s].child = (g->side[s].rti == va ? child_a : child_b) ? 0 : -1;
		g->side[s].inh = g->side[s].child < 0 && rte->inh &&
		                 (rte->relkind == RELKIND_PARTITIONED_TABLE || has_subclass(rte->relid));
	}
	for (int s = 0; s < 2; s++) {
		ListCell *lq;
		if (g->side[s].child >= 0) {
			continue;
		}
		foreach (lq, rt_fetch(g->side[s].rti, parse->rtable)->securityQuals) {
			Node *level = (Node *)lfirst(lq);
			List *qs = IsA(level, List) ? (List *)level : list_make1(level);
			ListCell *lq2;
			foreach (lq2, qs) {
				Node *q = (Node *)lfirst(lq2);
				if (contain_volatile_functions(q) || unsafe_qual_walker(q, NULL)) {
					return false;
				}
				g->side[s].quals = lappend(g->side[s].quals, copyObject(q));
			}
		}
	}
	const int rx = g->side[0].rti, ry = g->side[1].rti;
	foreach (lc, quals) {
		Node *q = (Node *)lfirst(lc);
		Relids rels = pull_varnos(root, q);
		const bool hx = bms_is_member(rx, rels), hy = bms_is_member(ry, rels);
		if (hx != hy) {
			TesseraSide *s = &g->side[hx ? 0 : 1];
			if (s->child >= 0) {
				continue;
			}
			if (bms_num_members(rels) != 1 || contain_volatile_functions(q) || unsafe_qual_walker(q, NULL)) {
				return false;
			}
			s->quals = lappend(s->quals, copyObject(q));
			continue;
		}
		const char *op;
		Var *x, *y;
		Oid coll;
		if (hx && bms_num_members(rels) == 2 && IsA(q, OpExpr) && list_length(((OpExpr *)q)->args) == 2 &&
		    !binop_cross(q, rx, ry, &op, &x, &y, &coll) && catalog_opname(((OpExpr *)q)->opno) &&
		    strcmp(catalog_opname(((OpExpr *)q)->opno), "=") == 0) {
			Node *e[2] = {(Node *)linitial(((OpExpr *)q)->args), (Node *)lsecond(((OpExpr *)q)->args)};
			int rel[2];
			for (int i = 0; i < 2; i++) {
				Relids r = pull_varnos(root, e[i]);
				rel[i] = bms_num_members(r) == 1 ? bms_singleton_member(r) : -1;
			}
			const int ix = rel[0] == rx ? 0 : 1;
			Node *ex = e[ix], *ey = e[1 - ix];
			Oid cx = exprCollation(ex), cy = exprCollation(ey);
			if (rel[ix] != rx || rel[1 - ix] != ry || g->side[0].nk == TESSERA_MAXCOLS ||
			    g->side[0].child >= 0 || g->side[1].child >= 0 || !keys_compatible(exprType(ex), exprType(ey)) ||
			    !key_ok(exprType(ex), cx) || !key_ok(exprType(ey), cy) ||
			    (!as_var(ex) && (!safe_group_expr(ex) || contain_volatile_functions(ex))) ||
			    (!as_var(ey) && (!safe_group_expr(ey) || contain_volatile_functions(ey)))) {
				return false;
			}
			const int c = g->side[0].nk;
			Node *ee[2] = {ex, ey};
			for (int i = 0; i < 2; i++) {
				Var *v = as_var(ee[i]);
				g->side[i].katt[c] = v ? v->varattno : 0;
				g->side[i].ktype[c] = exprType(ee[i]);
				g->side[i].kexpr[c] = v ? NULL : (Node *)copyObject(ee[i]);
			}
			g->side[0].nk = g->side[1].nk = c + 1;
			continue;
		}
		if (hx && bms_num_members(rels) == 2 && IsA(q, DistinctExpr) && list_length(((OpExpr *)q)->args) == 2) {
			Var *a0 = as_var((Node *)linitial(((OpExpr *)q)->args)), *a1 = as_var((Node *)lsecond(((OpExpr *)q)->args));
			Var *vx = a0 && a0->varno == rx ? a0 : a1, *vy = a0 && a0->varno == rx ? a1 : a0;
			const int fx = vx ? general_group(&g->side[0], vx) : -1, fy = vy ? general_group(&g->side[1], vy) : -1;
			const Oid dc = ((OpExpr *)q)->inputcollid;
			const char *dop = catalog_opname(((OpExpr *)q)->opno);
			if (!dop || strcmp(dop, "=") != 0 || !vx || !vy || vx->varno != rx || vy->varno != ry || fx < 0 || fy < 0 ||
			    g->nfilter == TESSERA_MAXCOLS || !(keys_compatible(vx->vartype, vy->vartype) ||
			      (text_type(vx->vartype) && vx->vartype == vy->vartype && vx->vartypmod == vy->vartypmod)) ||
			    (str_type(vx->vartype) && !(OidIsValid(dc) && get_collation_isdeterministic(dc)))) {
				return false;
			}
			g->fx[g->nfilter] = fx;
			g->fy[g->nfilter] = fy;
			g->fop[g->nfilter] = 5;
			g->fcoll[g->nfilter] = str_type(vx->vartype) ? dc : text_type(vx->vartype) ? C_COLLATION_OID : InvalidOid;
			g->nfilter++;
			continue;
		}
		if (!hx || bms_num_members(rels) != 2 || !binop_cross(q, rx, ry, &op, &x, &y, &coll)) {
			return false;
		}
		const bool text_xy = text_key_ok(x->vartype, x->vartypmod) && x->vartype == y->vartype && x->vartypmod == y->vartypmod;
		if (strcmp(op, "=") == 0) {
			if (g->side[0].nk == TESSERA_MAXCOLS ||
			    (!text_xy && (!key_ok(x->vartype, coll) || !key_ok(y->vartype, coll) || !keys_compatible(x->vartype, y->vartype)))) {
				return false;
			}
			const int c = g->side[0].nk;
			g->side[0].katt[c] = x->varattno;
			g->side[0].ktype[c] = x->vartype;
			g->side[1].katt[c] = y->varattno;
			g->side[1].ktype[c] = y->vartype;
			g->side[0].nk = g->side[1].nk = c + 1;
			continue;
		}
		const bool x_left = as_var(linitial(((OpExpr *)q)->args)) == x;
		int o = strcmp(op, "<") == 0 ? 0 : strcmp(op, "<=") == 0 ? 1 : strcmp(op, ">") == 0 ? 2 : strcmp(op, ">=") == 0 ? 3
		        : strcmp(op, "<>") == 0 ? 4 : -1;
		if (o >= 0 && o < 4 && !x_left) {
			o = o ^ 2;
		}
		const int fx = general_group(&g->side[0], x), fy = general_group(&g->side[1], y);
		if (o < 0 || fx < 0 || fy < 0 || g->nfilter == TESSERA_MAXCOLS || (!text_xy && !keys_compatible(x->vartype, y->vartype)) ||
		    (text_xy && o != 4) || (str_type(x->vartype) && !(OidIsValid(coll) && get_collation_isdeterministic(coll)))) {
			return false;
		}
		g->fx[g->nfilter] = fx;
		g->fy[g->nfilter] = fy;
		g->fop[g->nfilter] = o;
		g->fcoll[g->nfilter] = str_type(x->vartype) ? coll : text_xy ? C_COLLATION_OID : InvalidOid;
		g->nfilter++;
	}
	if (g->side[0].nk == 0) {
		return false;
	}
	for (int s = 0; s < 2; s++) {
		for (int i = 0; i < g->side[s].ng; i++) {
			if (!g->side[s].gexpr[i] && !att_not_null(g->side[s].relid, g->side[s].gatt[i])) {
				g->side[s].gnull[i] = true;
			}
		}
	}
	Aggref *aggs[TESSERA_MAXSUMS + 1];
	int naggs = 0;
	OutputItems oi = {g, NIL, true};
	foreach (lc, group_tlist(parse, output_rel)) {
		output_items_walker((Node *)lfirst_node(TargetEntry, lc)->expr, &oi);
	}
	if (!oi.ok) {
		return false;
	}
	foreach (lc, oi.items) {
		if (!IsA(lfirst(lc), Aggref)) {
			continue;
		}
		bool seen = false;
		for (int i = 0; i < naggs; i++) {
			seen = seen || equal(lfirst(lc), aggs[i]);
		}
		if (seen) {
			continue;
		}
		if (naggs == TESSERA_MAXSUMS + 1) {
			return false;
		}
		aggs[naggs++] = (Aggref *)lfirst(lc);
	}
	Aggref *first_sum = NULL;
	bool differ = false;
	for (int i = 0; i < naggs; i++) {
		const char *fn = get_func_namespace(aggs[i]->aggfnoid) == PG_CATALOG_NAMESPACE ? get_func_name(aggs[i]->aggfnoid) : NULL;
		if (fn && strcmp(fn, "sum") == 0) {
			differ = differ || (first_sum && !equal(first_sum->args, aggs[i]->args));
			first_sum = first_sum ? first_sum : aggs[i];
		}
	}
	Aggref *by_kind[4] = {NULL, NULL, NULL, NULL};
	g->null_ok = null_ok;
	if (differ || null_ok) {
		if (g->side[0].child >= 0 || g->side[1].child >= 0 || g->side[0].inh || g->side[1].inh) {
			return false;
		}
		for (int i = 0; i < naggs; i++) {
			const char *fn = get_func_namespace(aggs[i]->aggfnoid) == PG_CATALOG_NAMESPACE ? get_func_name(aggs[i]->aggfnoid) : NULL;
			if (aggs[i]->aggfilter || aggs[i]->aggdistinct || aggs[i]->aggorder || aggs[i]->aggkind != AGGKIND_NORMAL ||
			    aggs[i]->agglevelsup != 0) {
				return false;
			}
			if (fn && strcmp(fn, "count") == 0 && aggs[i]->aggstar) {
				if (by_kind[1]) {
					return false;
				}
				by_kind[1] = aggs[i];
				continue;
			}
			if (null_ok && fn && strcmp(fn, "count") == 0 && list_length(aggs[i]->args) == 1 && g->nmany < TESSERA_MAXSUMS) {
				Var *cv = as_var((Node *)linitial_node(TargetEntry, aggs[i]->args)->expr);
				if (!cv || cv->varlevelsup != 0 || cv->varattno <= 0 || (cv->varno != rx && cv->varno != ry)) {
					return false;
				}
				const int m = g->nmany++, cs = cv->varno == rx ? 0 : 1;
				g->many_count[m] = true;
				g->many_att[m][cs][g->many_nf[m][cs]++] = cv->varattno;
				g->many_null = g->many_null || !att_not_null(g->side[cs].relid, cv->varattno);
				g->many_aggs = lappend(g->many_aggs, aggs[i]);
				continue;
			}
			const bool avg = null_ok && fn && strcmp(fn, "avg") == 0 && aggs[i]->aggtype == NUMERICOID;
			if (null_ok && fn && (strcmp(fn, "sum") == 0 || avg) && list_length(aggs[i]->args) == 1 &&
			    g->nmany + (avg ? 1 : 0) < TESSERA_MAXSUMS && (g->nfilter == 0 || many_pair_op(g) != 0) &&
			    (aggs[i]->aggtype == INT8OID || aggs[i]->aggtype == NUMERICOID)) {
				Node *ae = (Node *)linitial_node(TargetEntry, aggs[i]->args)->expr;
				Var *iv[2] = {NULL, NULL};
				int bits = 1;
				bool prod = false;
				if (as_var(ae)) {
					iv[0] = as_var(ae);
				} else if (is_float8_mul(ae) && int_type(exprType(ae))) {
					prod = true;
					iv[0] = as_var((Node *)linitial(((OpExpr *)ae)->args));
					iv[1] = as_var((Node *)lsecond(((OpExpr *)ae)->args));
					bits = exprType(ae) == INT2OID ? 16 : exprType(ae) == INT4OID ? 32 : 64;
				}
				bool ok = iv[0] && (!prod || (iv[1] && (iv[0]->varno == rx) != (iv[1]->varno == rx)));
				for (int f = 0; f < 2 && ok; f++) {
					ok = !iv[f] || (iv[f]->varlevelsup == 0 && iv[f]->varattno > 0 && (iv[f]->varno == rx || iv[f]->varno == ry) &&
					               int_type(iv[f]->vartype));
				}
				if (!ok || (!avg && (aggs[i]->aggtype == INT8OID) != (iv[1] ? exprType(ae) != INT8OID : iv[0]->vartype != INT8OID))) {
					return false;
				}
				const int m = g->nmany++;
				g->many_bits[m] = bits;
				for (int f = 0; f < 2 && iv[f]; f++) {
					const int vs = iv[f]->varno == rx ? 0 : 1;
					g->many_att[m][vs][g->many_nf[m][vs]++] = iv[f]->varattno;
					g->many_null = g->many_null || !att_not_null(g->side[vs].relid, iv[f]->varattno);
				}
				g->many_aggs = lappend(g->many_aggs, aggs[i]);
				if (avg) {
					const int c = g->nmany++;
					g->many_avg[m] = true;
					g->many_count[c] = true;
					for (int s2 = 0; s2 < 2; s2++) {
						g->many_nf[c][s2] = g->many_nf[m][s2];
						for (int f = 0; f < g->many_nf[m][s2]; f++) {
							g->many_att[c][s2][f] = g->many_att[m][s2][f];
						}
					}
					g->many_aggs = lappend(g->many_aggs, makeConst(INT8OID, -1, InvalidOid, 8, Int64GetDatum(0), false, true));
				}
				continue;
			}
			if (!fn || strcmp(fn, "sum") != 0 || list_length(aggs[i]->args) != 1 || aggs[i]->aggtype != FLOAT8OID ||
			    g->nmany == TESSERA_MAXSUMS) {
				return false;
			}
			const int m = g->nmany++;
			if (!many_factors(g, m, (Node *)linitial_node(TargetEntry, aggs[i]->args)->expr, rx, ry)) {
				return false;
			}
			g->many_aggs = lappend(g->many_aggs, aggs[i]);
		}
		if (null_ok && !g->many_null) {
			return false;
		}
		g->agg = by_kind[1];
	} else {
		for (int i = 0; i < naggs; i++) {
			const int k = general_aggregate(g, aggs[i], rx, ry);
			if (!k || by_kind[k]) {
				return false;
			}
			by_kind[k] = aggs[i];
		}
		if (naggs == 0 || (by_kind[2] && by_kind[3] && !equal(by_kind[2]->args, by_kind[3]->args)) ||
		    (by_kind[3] && g->distinct)) {
			return false;
		}
		g->two = naggs >= 2 || by_kind[3];
		g->agg = g->two ? by_kind[1] : aggs[0];
		g->agg2 = g->two ? by_kind[2] : NULL;
		g->agg3 = by_kind[3];
	}
	g->aggtype = g->agg ? g->agg->aggtype : INT8OID;
	g->aggtype2 = g->agg2 ? g->agg2->aggtype : g->two ? NUMERICOID : InvalidOid;
	g->avgtype = g->agg3 ? g->agg3->aggtype : InvalidOid;
	List *having = (List *)parse->havingQual;
	if (having) {
		GeneralRefs hr = {g, true};
		if (general_refs_walker((Node *)having, &hr) || !hr.ok) {
			return false;
		}
	}
	if (having && g->agg) {
		TesseraMatch tm;
		memset(&tm, 0, sizeof tm);
		tm.agg = g->agg;
		having_prune(having, &tm);
		g->minval = tm.p[P_MINVAL];
	}
	if (g->nfilter == 0 && !parse->hasWindowFuncs && g->agg) {
		TesseraMatch tm;
		memset(&tm, 0, sizeof tm);
		tm.agg = g->agg;
		limit_prune(parse, &tm);
		g->topk = tm.p[P_TOPK];
	}
	if (g->nfilter == 0 && parse->hasWindowFuncs && g->agg && !g->two && !g->agg2 && !g->agg3 && g->nmany == 0) {
		List *part = NIL;
		const int64 k = rank_limit(parse, (Node *)g->agg, &part);
		if (k > 0) {
			int side = -1, n = 0;
			ListCell *lp;
			foreach (lp, part) {
				Node *e = (Node *)get_sortgroupclause_expr(lfirst_node(SortGroupClause, lp), parse->targetList);
				const int s0 = general_group_node(&g->side[0], e) >= 0 ? 0 : general_group_node(&g->side[1], e) >= 0 ? 1 : -1;
				if (s0 < 0 || (side >= 0 && s0 != side)) {
					side = -2;
					break;
				}
				side = s0;
				n++;
			}
			if (side >= 0 && n == g->side[side].ng) {
				g->group_topk = k;
				g->group_side = side;
			}
		}
	}
	const TesseraSide *x = &g->side[0], *y = &g->side[1];
	bool sym = g->nmany == 0 && x->child < 0 && y->child < 0 && x->relid == y->relid && x->inh == y->inh && x->nk == y->nk &&
	           x->ng == y->ng && x->vatt == y->vatt &&
	           x->vcast == y->vcast &&
	           (x->vatt != 0) == g->has_v && memcmp(x->katt, y->katt, sizeof(AttrNumber) * x->nk) == 0 &&
	           memcmp(x->gatt, y->gatt, sizeof(AttrNumber) * x->ng) == 0 && same_computed(x, y);
	if (sym) {
		List *qy = (List *)copyObject(y->quals);
		ChangeVarNodes((Node *)qy, y->rti, x->rti, 0);
		sym = equal(qy, x->quals);
	}
	g->symmetric = sym;
	for (int s = 0; s < 2; s++) {
		const TesseraSide *d = &g->side[s];
		if (!d->inh) {
			continue;
		}
		Bitmapset *att = NULL;
		pull_varattnos((Node *)d->quals, d->rti, &att);
		for (int c = 0; c < d->ng; c++) {
			pull_varattnos(d->gexpr[c], d->rti, &att);
		}
		for (int c = 0; c < d->nk; c++) {
			pull_varattnos(d->kexpr[c], d->rti, &att);
		}
		if (bms_is_member(0 - FirstLowInvalidHeapAttributeNumber, att)) {
			return false;
		}
	}
	return true;
}

static bool general_expands(PlannerInfo *root, const TesseraGeneral *g) {
	const double theta = tessera_min_expansion_cross;
	const TesseraSide *x = &g->side[0], *y = &g->side[1];
	const double na = rel_rows(root, x->rti), da = x->kexpr[0] ? -1 : key_distinct(root, x->rti, x->katt[0], x->ktype[0]);
	const double nb = rel_rows(root, y->rti), db = y->kexpr[0] ? -1 : key_distinct(root, y->rti, y->katt[0], y->ktype[0]);
	const double e = na > 0 && da > 0 && nb > 0 && db > 0 ? na * nb / (Max(da, db) * (na + nb)) : -1;
	const bool run = theta <= 0 || e < 0 || e >= theta;
	if (tessera_verbose) {
		elog(LOG, "tessera: %.3g joined rows per input row (estimate), limit %.3g: %s", e, theta,
		     run ? "Tessera" : "PostgreSQL's plan");
	}
	return run;
}

static List *general_to_list(const TesseraGeneral *g) {
	List *scal = NIL, *filt = NIL;
	int64 v[] = {g->nfilter, g->has_v,    g->distinct, g->aggtype,   g->minval,
	             g->topk,    g->two,      g->aggtype2, g->symmetric, g->avgtype,
	             g->group_topk, g->group_side};
	List *many = list_make2_int(g->nmany, g->many_null);
	for (int m = 0; m < g->nmany; m++) {
		many = lappend_int(many, g->many_count[m]);
		many = lappend_int(many, g->many_bits[m]);
		many = lappend_int(many, g->many_avg[m]);
		for (int s = 0; s < 2; s++) {
			many = lappend_int(many, g->many_nf[m][s]);
			for (int f = 0; f < g->many_nf[m][s]; f++) {
				many = lappend_int(many, g->many_att[m][s][f]);
			}
		}
	}
	for (int i = 0; i < (int)lengthof(v); i++) {
		scal = lappend(scal, makeConst(INT8OID, -1, InvalidOid, 8, Int64GetDatum(v[i]), false, true));
	}
	for (int f = 0; f < g->nfilter; f++) {
		filt = lappend_int(lappend_int(lappend_int(lappend_int(filt, g->fx[f]), g->fy[f]), g->fop[f]), (int)g->fcoll[f]);
	}
	List *out = list_make2(scal, filt);
	for (int s = 0; s < 2; s++) {
		const TesseraSide *d = &g->side[s];
		List *katt = NIL, *gatt = NIL, *ktype = NIL, *gtype = NIL, *gnull = NIL, *gexpr = NIL;
		for (int i = 0; i < d->nk; i++) {
			katt = lappend_int(katt, d->katt[i]);
			ktype = lappend_oid(ktype, d->ktype[i]);
		}
		for (int i = 0; i < d->ng; i++) {
			gatt = lappend_int(gatt, d->gatt[i]);
			gtype = lappend_oid(gtype, d->gtype[i]);
			gnull = lappend_int(gnull, d->gnull[i]);
			gexpr = lappend(gexpr, d->gexpr[i]);
		}
		List *misc = lappend_int(lappend_int(lappend_int(list_make4_int(d->rti, (int)d->relid, d->vatt, (int)d->vtype), d->inh),
		                                     d->vtypmod),
		                         d->vcast);
		misc = lappend_int(lappend_int(misc, d->child), d->vpos);
		List *kpos = NIL, *gpos = NIL;
		for (int i = 0; i < d->nk; i++) {
			kpos = lappend_int(kpos, d->kpos[i]);
		}
		for (int i = 0; i < d->ng; i++) {
			gpos = lappend_int(gpos, d->gpos[i]);
		}
		List *kexpr = NIL;
		for (int i = 0; i < d->nk; i++) {
			kexpr = lappend(kexpr, d->kexpr[i]);
		}
		out = lappend(out, list_make5(misc, katt, ktype, gatt,
		                              lappend(lappend(lappend(list_make4(gtype, d->quals, gnull, gexpr), kpos), gpos), kexpr)));
	}
	out = lappend(lappend(out, many), g->many_aggs);
	return out;
}

static void general_from_list(List *l, TesseraGeneral *g) {
	memset(g, 0, sizeof *g);
	List *scal = linitial(l), *filt = lsecond(l);
	int64 v[12];
	int i = 0;
	ListCell *lc;
	foreach (lc, scal) {
		Assert(i < (int)lengthof(v));
		v[i++] = DatumGetInt64(lfirst_node(Const, lc)->constvalue);
	}
	g->nfilter = (int)v[0];
	g->has_v = v[1] != 0;
	g->distinct = v[2] != 0;
	g->aggtype = (Oid)v[3];
	g->minval = v[4];
	g->topk = v[5];
	g->two = v[6] != 0;
	g->aggtype2 = (Oid)v[7];
	g->symmetric = v[8] != 0;
	g->avgtype = (Oid)v[9];
	g->group_topk = v[10];
	g->group_side = (int)v[11];
	{
		List *many = list_nth(l, list_length(l) - 2);
		int pos = 0;
		g->nmany = list_nth_int(many, pos++);
		g->many_null = list_nth_int(many, pos++) != 0;
		for (int m = 0; m < g->nmany; m++) {
			g->many_count[m] = list_nth_int(many, pos++) != 0;
			g->many_bits[m] = list_nth_int(many, pos++);
			g->many_avg[m] = list_nth_int(many, pos++) != 0;
			for (int s = 0; s < 2; s++) {
				g->many_nf[m][s] = list_nth_int(many, pos++);
				for (int f = 0; f < g->many_nf[m][s]; f++) {
					g->many_att[m][s][f] = (AttrNumber)list_nth_int(many, pos++);
				}
			}
		}
		g->many_aggs = llast(l);
	}
	for (int f = 0; f < g->nfilter; f++) {
		g->fx[f] = list_nth_int(filt, 4 * f);
		g->fy[f] = list_nth_int(filt, 4 * f + 1);
		g->fop[f] = list_nth_int(filt, 4 * f + 2);
		g->fcoll[f] = (Oid)list_nth_int(filt, 4 * f + 3);
	}
	for (int s = 0; s < 2; s++) {
		List *d = list_nth(l, 2 + s), *misc = linitial(d), *katt = lsecond(d), *ktype = lthird(d), *gatt = lfourth(d);
		List *tail = list_nth(d, 4);
		TesseraSide *t = &g->side[s];
		t->rti = list_nth_int(misc, 0);
		t->relid = (Oid)list_nth_int(misc, 1);
		t->vatt = (AttrNumber)list_nth_int(misc, 2);
		t->vtype = (Oid)list_nth_int(misc, 3);
		t->inh = list_nth_int(misc, 4) != 0;
		t->vtypmod = list_nth_int(misc, 5);
		t->vcast = list_nth_int(misc, 6) != 0;
		t->child = list_nth_int(misc, 7);
		t->vpos = (AttrNumber)list_nth_int(misc, 8);
		t->nk = list_length(katt);
		for (int c = 0; c < t->nk; c++) {
			t->katt[c] = (AttrNumber)list_nth_int(katt, c);
			t->ktype[c] = list_nth_oid(ktype, c);
			t->kpos[c] = (AttrNumber)list_nth_int(list_nth(tail, 4), c);
			t->kexpr[c] = (Node *)list_nth(list_nth(tail, 6), c);
		}
		t->ng = list_length(gatt);
		for (int c = 0; c < t->ng; c++) {
			t->gatt[c] = (AttrNumber)list_nth_int(gatt, c);
			t->gtype[c] = list_nth_oid(linitial(tail), c);
			t->gnull[c] = list_nth_int(lthird(tail), c) != 0;
			t->gexpr[c] = (Node *)list_nth(lfourth(tail), c);
			t->gpos[c] = (AttrNumber)list_nth_int(list_nth(tail, 5), c);
		}
		t->quals = lsecond(tail);
	}
}

static void tessera_upper_hook(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel,
                               RelOptInfo *output_rel, void *extra) {
	if (prev_upper_hook) {
		prev_upper_hook(root, stage, input_rel, output_rel, extra);
	}
	TesseraMatch m;
	TesseraGeneral g;
	List *priv, *children = NIL;
	if (!tessera_enabled || (stage != UPPERREL_GROUP_AGG && stage != UPPERREL_DISTINCT) || !tessera_c_supported()) {
		return;
	}
	if (match_query(root, stage, output_rel, &m)) {
		if (!join_expands(root, &m)) {
			return;
		}
		priv = list_make2(params_list(m.p), makeInteger(m.va));
	} else if (match_general(root, stage, output_rel, &g, false) || match_general(root, stage, output_rel, &g, true)) {
		if (!general_expands(root, &g)) {
			return;
		}
		for (int s = 0; s < 2; s++) {
			if (g.side[s].child >= 0) {
				g.side[s].child = list_length(children);
				children = lappend(children, find_base_rel(root, g.side[s].rti)->cheapest_total_path);
			}
		}
		memset(m.p, 0, sizeof m.p);
		priv = list_make3(params_list(m.p), makeInteger(g.side[0].rti), general_to_list(&g));
	} else {
		return;
	}
	CustomPath *cp = makeNode(CustomPath);
	cp->path.pathtype = T_CustomScan;
	cp->path.parent = output_rel;
	cp->path.pathtarget = stage == UPPERREL_DISTINCT ? input_rel->reltarget : output_rel->reltarget;
	cp->path.param_info = NULL;
	cp->path.parallel_aware = false;
	cp->path.parallel_safe = false;
	cp->path.parallel_workers = 0;
	double rows = output_rel->rows;
	if (rows <= 0 && output_rel->pathlist != NIL) {
		rows = ((Path *)linitial(output_rel->pathlist))->rows;
	}
	cp->path.rows = rows > 0 ? rows : 1;
	cp->path.startup_cost = 0;
	cp->path.total_cost = 1;
	cp->path.pathkeys = NIL;
	cp->flags = 0;
	cp->custom_paths = children;
	cp->custom_private = priv;
	cp->methods = &tessera_path_methods;
	add_path(output_rel, &cp->path);
}

static AttrNumber tlist_position(List *tl, int rti, AttrNumber att) {
	ListCell *lc;
	foreach (lc, tl) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		Var *v = as_var((Node *)te->expr);
		if (v && (int)v->varno == rti && v->varattno == att) {
			return te->resno;
		}
	}
	elog(ERROR, "tessera: a column of an input is missing from its plan's output");
	return 0;
}

static bool agg_is(Expr *e, const char *name) {
	char *fn = get_func_name(((Aggref *)e)->aggfnoid);
	return fn && strcmp(fn, name) == 0;
}

static Plan *tessera_plan_path(PlannerInfo *root, RelOptInfo *rel, struct CustomPath *best_path, List *tlist,
                               List *clauses, List *custom_plans) {
	CustomScan *cs = makeNode(CustomScan);
	List *params = linitial(best_path->custom_private);
	int va = intVal(lsecond(best_path->custom_private));
	const bool general = list_length(best_path->custom_private) == 3;
	TesseraGeneral g;
	if (general) {
		general_from_list(lthird(best_path->custom_private), &g);
	}
	List *scan_tlist = tlist;
	if (general) {
		OutputItems oi = {&g, NIL, true};
		ListCell *li;
		foreach (li, tlist) {
			output_items_walker((Node *)lfirst_node(TargetEntry, li)->expr, &oi);
		}
		output_items_walker(root->parse->havingQual, &oi);
		if (!oi.ok) {
			elog(ERROR, "tessera: unexpected expression in the grouping target");
		}
		scan_tlist = NIL;
		foreach (li, oi.items) {
			scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *)copyObject(lfirst(li)), list_length(scan_tlist) + 1, NULL, false));
		}
	}
	List *kinds = NIL;
	ListCell *lc;
	foreach (lc, scan_tlist) {
		TargetEntry *te = lfirst_node(TargetEntry, lc);
		Var *v = as_var((Node *)te->expr);
		int kind;
		if (IsA(te->expr, Aggref)) {
			kind = !general || !g.two ? 2 : agg_is(te->expr, "sum") ? 3 : agg_is(te->expr, "avg") ? 4 : 2;
			if (general && g.nmany > 0 && (agg_is(te->expr, "sum") || !((Aggref *)te->expr)->aggstar)) {
				kind = -1;
				for (int m = 0; m < g.nmany; m++) {
					kind = equal(te->expr, list_nth(g.many_aggs, m)) ? 300 + m : kind;
				}
			}
		} else if (general) {
			const int i = general_group_node(&g.side[0], (Node *)te->expr), j = general_group_node(&g.side[1], (Node *)te->expr);
			kind = i >= 0 ? 100 + i : j >= 0 ? 200 + j : -1;
		} else {
			kind = (v && v->varno == va) ? 0 : (v ? 1 : -1);
		}
		if (kind < 0) {
			elog(ERROR, "tessera: unexpected expression in the grouping target");
		}
		kinds = lappend_int(kinds, kind);
	}
	cs->scan.plan.targetlist = tlist;
	cs->scan.plan.qual = (List *)copyObject(root->parse->havingQual);
	cs->scan.scanrelid = 0;
	cs->flags = best_path->flags;
	cs->custom_plans = NIL;
	cs->custom_exprs = NIL;
	List *gl = general ? lthird(best_path->custom_private) : NIL;
	if (general && custom_plans) {
		for (int s = 0; s < 2; s++) {
			TesseraSide *d = &g.side[s];
			if (d->child < 0) {
				continue;
			}
			List *ctl = ((Plan *)list_nth(custom_plans, d->child))->targetlist;
			for (int c = 0; c < d->nk; c++) {
				d->kpos[c] = tlist_position(ctl, d->rti, d->katt[c]);
			}
			for (int c = 0; c < d->ng; c++) {
				d->gpos[c] = tlist_position(ctl, d->rti, d->gatt[c]);
			}
			d->vpos = d->vatt ? tlist_position(ctl, d->rti, d->vatt) : 0;
		}
		gl = general_to_list(&g);
		cs->custom_plans = custom_plans;
	}
	cs->custom_private = general ? list_make3(params, kinds, gl) : list_make2(params, kinds);
	cs->custom_scan_tlist = (List *)copyObject(scan_tlist);
	cs->custom_relids = NULL;
	cs->methods = &tessera_scan_methods;
	return &cs->scan.plan;
}

typedef struct TesseraState {
	CustomScanState css;
	int64 p[P_NPARAM];
	int *kind;
	bool general;
	TesseraGeneral g;
	int ncol;
	tessera_query_c *q;
	tessera_dict_c *dk, *dx, *dy;
	bool ran;
	bool wide;
	bool nonfinite;
	bool many_nonfinite[TESSERA_MAXSUMS];
	HTAB *nan_keys[2], *nan_cells, *collect;
	int64 *coll_k, *coll_x;
	bool *coll_nan;
	int coll_n, coll_cap;
	uint64 cyc_next, cyc_groups, cyc_cols, trace_rows;
	int64 *rb_x0, *rb_x1, *rb_vi, *rb_cnt, *rb_hi;
	uint64 *rb_lo;
	double *rb_vd;
	double *rb_many;
	uint64_t *rb_nulls;
	int64_t *rb_isums;
	int rb_n, rb_pos, rb_cap, rb_dict[2];
	bool rb_ready, rb_general;
	MemoryContextCallback release;
} TesseraState;

static void tessera_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *tessera_exec(CustomScanState *node);
static void tessera_end(CustomScanState *node);
static void tessera_rescan(CustomScanState *node);
static void tessera_explain(CustomScanState *node, List *ancestors, ExplainState *es);

static const CustomExecMethods tessera_exec_methods = {
    .CustomName = "Tessera",
    .BeginCustomScan = tessera_begin,
    .ExecCustomScan = tessera_exec,
    .EndCustomScan = tessera_end,
    .ReScanCustomScan = tessera_rescan,
    .ExplainCustomScan = tessera_explain,
};

static Node *tessera_create_state(CustomScan *cscan) {
	TesseraState *s = (TesseraState *)newNode(sizeof(TesseraState), T_CustomScanState);
	List *params = linitial(cscan->custom_private), *kinds = lsecond(cscan->custom_private);
	ListCell *lc;
	int i = 0;
	foreach (lc, params) {
		s->p[i++] = DatumGetInt64(lfirst_node(Const, lc)->constvalue);
	}
	s->ncol = 0;
	s->kind = palloc(sizeof(int) * Max(list_length(kinds), 1));
	foreach (lc, kinds) {
		s->kind[s->ncol++] = lfirst_int(lc);
	}
	s->general = list_length(cscan->custom_private) == 3;
	if (s->general) {
		general_from_list(lthird(cscan->custom_private), &s->g);
	}
	s->css.methods = &tessera_exec_methods;
	return (Node *)s;
}

static void tessera_release(void *arg) {
	TesseraState *s = (TesseraState *)arg;
	if (s->q) {
		tessera_c_free(s->q);
		s->q = NULL;
	}
	if (s->dk) {
		tessera_dict_free(s->dk);
		s->dk = NULL;
	}
	if (s->dx) {
		tessera_dict_free(s->dx);
		s->dx = NULL;
	}
	if (s->dy) {
		tessera_dict_free(s->dy);
		s->dy = NULL;
	}
	s->nan_keys[0] = s->nan_keys[1] = s->nan_cells = s->collect = NULL;
	s->coll_n = 0;
}

static void tessera_begin(CustomScanState *node, EState *estate, int eflags) {
	TesseraState *s = (TesseraState *)node;
	ListCell *lc;
	foreach (lc, ((CustomScan *)node->ss.ps.plan)->custom_plans) {
		node->custom_ps = lappend(node->custom_ps, ExecInitNode((Plan *)lfirst(lc), estate, eflags));
	}
	s->q = NULL;
	s->dk = s->dx = s->dy = NULL;
	s->ran = false;
	s->wide = false;
	s->release.func = tessera_release;
	s->release.arg = s;
	MemoryContextRegisterResetCallback(estate->es_query_cxt, &s->release);
}

static int64 datum_int(Datum d, Oid t);

static int64 datum_key(Datum d, Oid t, tessera_dict_c *dict) {
	if (str_type(t)) {
		text *v = DatumGetTextPP(d);
		int64 c = tessera_dict_code(dict, VARDATA_ANY(v), VARSIZE_ANY_EXHDR(v));
		if (c < 0) {
			elog(ERROR, "tessera: out of memory");
		}
		return c;
	}
	return datum_int(d, t);
}

static int pg_should_stop(void *ctx) {
	return QueryCancelPending || ProcDiePending;
}

static int64 datum_int(Datum d, Oid t) {
	switch (t) {
	case INT8OID: return DatumGetInt64(d);
	case INT4OID: return DatumGetInt32(d);
	case INT2OID: return DatumGetInt16(d);
	case DATEOID: return DatumGetDateADT(d);
	default: return DatumGetTimestamp(d);
	}
}

static Datum int_datum(int64 x, Oid t) {
	switch (t) {
	case INT8OID: return Int64GetDatum(x);
	case INT4OID: return Int32GetDatum((int32)x);
	case INT2OID: return Int16GetDatum((int16)x);
	case DATEOID: return DateADTGetDatum((DateADT)x);
	default: return TimestampGetDatum((Timestamp)x);
	}
}

static double datum_double(Datum d, Oid t) {
	return t == FLOAT8OID    ? DatumGetFloat8(d)
	       : t == FLOAT4OID  ? (double)DatumGetFloat4(d)
	       : t == NUMERICOID ? DatumGetFloat8(DirectFunctionCall1(numeric_float8, d))
	                         : (double)datum_int(d, t);
}

static int64 numeric_scaled(Datum v, Datum pow) {
	return DatumGetInt64(DirectFunctionCall1(numeric_int8, DirectFunctionCall2(numeric_mul, v, pow)));
}

static void scan_rel(tessera_query_c *q, tessera_dict_c *dk, tessera_dict_c *dxs, Oid relid, AttrNumber ka,
                     AttrNumber xa, AttrNumber vatt, Oid kt, Oid xt, Oid vt, int side, bool int_v, bool *nonfinite) {
	const bool has_v = vatt != 0;
	MemoryContext rowcxt = AllocSetContextCreate(CurrentMemoryContext, "tessera scan", ALLOCSET_DEFAULT_SIZES);
	Relation rel = table_open(relid, AccessShareLock);
	TupleTableSlot *slot = table_slot_create(rel, NULL);
	TableScanDesc sd = table_beginscan(rel, GetActiveSnapshot(), 0, NULL);
	enum { BATCH = 65536 };
	int64 *kb = palloc(sizeof(int64) * BATCH), *xb = palloc(sizeof(int64) * BATCH);
	double *vb = has_v && !int_v ? palloc(sizeof(double) * BATCH) : NULL;
	int64 *vib = has_v && int_v ? palloc(sizeof(int64) * BATCH) : NULL;
	int n = 0;
#define TESSERA_ADD()                                                                                                 \
	(int_v ? (side ? tessera_c_add_y_i64(q, kb, xb, (const int64_t *)vib, n) : tessera_c_add_i64(q, kb, xb, (const int64_t *)vib, n)) \
	       : (side ? tessera_c_add_y(q, kb, xb, vb, n) : tessera_c_add(q, kb, xb, vb, n)))
	while (table_scan_getnextslot(sd, ForwardScanDirection, slot)) {
		bool kn, xn, vn = false;
		Datum kd = slot_getattr(slot, ka, &kn), xd = slot_getattr(slot, xa, &xn), vd = 0;
		if (has_v) {
			vd = slot_getattr(slot, vatt, &vn);
		}
		if (kn) {
			continue;
		}
		if (xn || vn) {
			elog(ERROR, "tessera: NULL group keys or values");
		}
		MemoryContext old = MemoryContextSwitchTo(rowcxt);
		kb[n] = datum_key(kd, kt, dk);
		xb[n] = datum_key(xd, xt, dxs);
		MemoryContextSwitchTo(old);
		if (has_v && int_v) {
			vib[n] = datum_int(vd, vt);
		} else if (has_v) {
			vb[n] = datum_double(vd, vt);
			if (nonfinite && !isfinite(vb[n])) {
				*nonfinite = true;
			}
		}
		if (++n == BATCH) {
			if (TESSERA_ADD() != 0) {
				elog(ERROR, "tessera: out of memory");
			}
			n = 0;
			MemoryContextReset(rowcxt);
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (n && TESSERA_ADD() != 0) {
		elog(ERROR, "tessera: out of memory");
	}
#undef TESSERA_ADD
	table_endscan(sd);
	ExecDropSingleTupleTableSlot(slot);
	table_close(rel, NoLock);
	pfree(kb);
	pfree(xb);
	if (vb) {
		pfree(vb);
	}
	if (vib) {
		pfree(vib);
	}
	MemoryContextDelete(rowcxt);
}

static void set_spill(tessera_query_c *q) {
	char dir[MAXPGPATH];
	PrepareTempTablespaces();
	TempTablespacePath(dir, GetNextTempTableSpace());
	(void)MakePGDirectory(dir);
	tessera_c_set_memory(q, get_hash_memory_limit(), dir);
	if (temp_file_limit >= 0) {
		tessera_c_set_spill_limit(q, (uint64_t)temp_file_limit * 1024);
	}
}

static void run_error(int rc, const char *err) {
	if (rc == TESSERA_C_CANCELLED) {
		CHECK_FOR_INTERRUPTS();
	}
	if (rc == TESSERA_C_OUT_OF_MEMORY) {
		ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory"), errdetail("%s", err)));
	}
	if (rc == TESSERA_C_SPILL_LIMIT) {
		ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
		                errmsg("temporary file size exceeds \"temp_file_limit\" (%dkB)", temp_file_limit),
		                errdetail("%s", err)));
	}
	if (rc == TESSERA_C_SPILL_ERROR) {
		ereport(ERROR, (errcode(ERRCODE_IO_ERROR), errmsg("%s", err)));
	}
	elog(ERROR, "%s", err);
}

static int int_bits(Oid t) {
	return t == INT2OID ? 16 : t == INT4OID ? 32 : 64;
}

static Datum numeric_of_i128_scaled(int64_t hi, uint64_t lo, int scale) {
	if (scale == 0 && hi == ((lo >> 63) ? -1 : 0)) {
		return NumericGetDatum(int64_to_numeric((int64)lo));
	}
	__int128 v = (__int128)(((unsigned __int128)(uint64_t)hi << 64) | lo);
	char buf[96], *p = buf + sizeof buf - 1;
	const bool neg = v < 0;
	unsigned __int128 u = neg ? (unsigned __int128)0 - (unsigned __int128)v : (unsigned __int128)v;
	*p = 0;
	int digits = 0;
	do {
		*--p = (char)('0' + (int)(u % 10));
		u /= 10;
		if (++digits == scale) {
			*--p = '.';
		}
	} while (u || digits <= scale);
	if (neg) {
		*--p = '-';
	}
	return DirectFunctionCall3(numeric_in, CStringGetDatum(p), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
}

static Datum numeric_of_i128(int64_t hi, uint64_t lo) {
	return numeric_of_i128_scaled(hi, lo, 0);
}

static void tuple_append(StringInfo b, Datum d, Oid t) {
	if (bytes_type(t)) {
		const char *p;
		uint32 len;
		value_bytes(d, t, &p, &len);
		appendBinaryStringInfo(b, (const char *)&len, sizeof len);
		appendBinaryStringInfo(b, p, len);
	} else {
		const int64 x = datum_int(d, t);
		appendBinaryStringInfo(b, (const char *)&x, sizeof x);
	}
}

static int64 columns_code(TupleTableSlot *slot, int n, const AttrNumber *att, const Oid *type, tessera_dict_c *d,
                          StringInfo buf, bool *null, ExprState **estate, ExprContext *ec) {
	*null = false;
	ec->ecxt_scantuple = slot;
	if (n == 1 && !bytes_type(type[0])) {
		Datum v = estate[0] ? ExecEvalExpr(estate[0], ec, null) : slot_getattr(slot, att[0], null);
		return *null ? 0 : datum_int(v, type[0]);
	}
	resetStringInfo(buf);
	for (int c = 0; c < n; c++) {
		Datum v = estate[c] ? ExecEvalExpr(estate[c], ec, null) : slot_getattr(slot, att[c], null);
		if (*null) {
			return 0;
		}
		if (n == 1) {
			const char *p;
			uint32 len;
			value_bytes(v, type[0], &p, &len);
			appendBinaryStringInfo(buf, p, len);
		} else {
			tuple_append(buf, v, type[c]);
		}
	}
	const int64 code = tessera_dict_code(d, buf->data, buf->len);
	if (code < 0) {
		elog(ERROR, "tessera: out of memory");
	}
	return code;
}

static bool general_dict(int n, const Oid *type) {
	return n > 1 || bytes_type(type[0]);
}

static int64 group_code(TupleTableSlot *slot, const TesseraSide *d, ExprState **gstate, ExprContext *ec,
                        tessera_dict_c *dg, StringInfo buf, bool *null) {
	*null = false;
	if (!group_dict(d) && !d->gexpr[0]) {
		Datum v = slot_getattr(slot, d->gatt[0], null);
		return *null ? 0 : datum_int(v, d->gtype[0]);
	}
	resetStringInfo(buf);
	ec->ecxt_scantuple = slot;
	for (int c = 0; c < d->ng; c++) {
		bool isnull;
		Datum v = d->gexpr[c] ? ExecEvalExpr(gstate[c], ec, &isnull) : slot_getattr(slot, d->gatt[c], &isnull);
		if (d->gnull[c]) {
			appendStringInfoChar(buf, isnull ? 0 : 1);
			if (isnull) {
				continue;
			}
		} else if (isnull) {
			*null = true;
			return 0;
		}
		if (!group_dict(d)) {
			return datum_int(v, d->gtype[0]);
		}
		if (!group_tuples(d)) {
			const char *p;
			uint32 len;
			value_bytes(v, d->gtype[0], &p, &len);
			appendBinaryStringInfo(buf, p, len);
		} else {
			tuple_append(buf, v, d->gtype[c]);
		}
	}
	const int64 code = tessera_dict_code(dg, buf->data, buf->len);
	if (code < 0) {
		elog(ERROR, "tessera: out of memory");
	}
	return code;
}

static void scan_one(TesseraState *s, const TesseraSide *d, Relation rel, PlanState *child, int side, bool int_v,
                     tessera_dict_c *dg);

static void nan_collect(TesseraState *s, int64 k, int64 x, bool nan) {
	if (s->coll_n == s->coll_cap) {
		MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.state->es_query_cxt);
		s->coll_cap = s->coll_cap ? 2 * s->coll_cap : 1024;
		s->coll_k = s->coll_k ? repalloc_huge(s->coll_k, sizeof(int64) * s->coll_cap)
		                      : palloc_extended(sizeof(int64) * s->coll_cap, MCXT_ALLOC_HUGE);
		s->coll_x = s->coll_x ? repalloc_huge(s->coll_x, sizeof(int64) * s->coll_cap)
		                      : palloc_extended(sizeof(int64) * s->coll_cap, MCXT_ALLOC_HUGE);
		s->coll_nan = s->coll_nan ? repalloc_huge(s->coll_nan, sizeof(bool) * s->coll_cap)
		                          : palloc_extended(sizeof(bool) * s->coll_cap, MCXT_ALLOC_HUGE);
		MemoryContextSwitchTo(old);
	}
	s->coll_k[s->coll_n] = k;
	s->coll_x[s->coll_n] = x;
	s->coll_nan[s->coll_n] = nan;
	s->coll_n++;
}

static void scan_side(TesseraState *s, const TesseraSide *d, int side, bool int_v, tessera_dict_c *dg) {
	if (d->child >= 0) {
		TesseraSide m = *d;
		for (int c = 0; c < m.nk; c++) {
			m.katt[c] = m.kpos[c];
		}
		for (int c = 0; c < m.ng; c++) {
			m.gatt[c] = m.gpos[c];
		}
		m.vatt = m.vpos;
		m.quals = NIL;
		scan_one(s, &m, NULL, (PlanState *)list_nth(s->css.custom_ps, d->child), side, int_v, dg);
		return;
	}
	List *rels = d->inh ? find_all_inheritors(d->relid, AccessShareLock, NULL) : list_make1_oid(d->relid);
	Relation top = table_open(d->relid, AccessShareLock);
	ListCell *lc;
	foreach (lc, rels) {
		Relation rel = table_open(lfirst_oid(lc), AccessShareLock);
		if (rel->rd_rel->relkind != RELKIND_RELATION || (RelationGetRelid(rel) != d->relid && RELATION_IS_OTHER_TEMP(rel))) {
			table_close(rel, NoLock);
			continue;
		}
		TesseraSide m = *d;
		if (RelationGetRelid(rel) != d->relid) {
			AttrMap *map = build_attrmap_by_name(RelationGetDescr(rel), RelationGetDescr(top), false);
			bool whole_row = false;
			for (int c = 0; c < m.nk; c++) {
				if (m.kexpr[c]) {
					m.kexpr[c] = map_variable_attnos(d->kexpr[c], d->rti, 0, map, InvalidOid, &whole_row);
				} else {
					m.katt[c] = map->attnums[m.katt[c] - 1];
				}
			}
			for (int c = 0; c < m.ng; c++) {
				if (m.gexpr[c]) {
					m.gexpr[c] = map_variable_attnos(d->gexpr[c], d->rti, 0, map, InvalidOid, &whole_row);
				} else {
					m.gatt[c] = map->attnums[m.gatt[c] - 1];
				}
			}
			if (m.vatt) {
				m.vatt = map->attnums[m.vatt - 1];
			}
			m.quals = (List *)map_variable_attnos((Node *)d->quals, d->rti, 0, map, InvalidOid, &whole_row);
			if (whole_row) {
				elog(ERROR, "tessera: a whole-row reference in a filter or group of an inherited table");
			}
		}
		scan_one(s, &m, rel, NULL, side, int_v, dg);
		table_close(rel, NoLock);
	}
	table_close(top, NoLock);
}

typedef struct SideRows {
	const TesseraSide *d;
	int side;
	tessera_dict_c *dg;
	ExprState *gstate[TESSERA_MAXCOLS], *kstate[TESSERA_MAXCOLS];
	int64 *kb, *xb, *vib;
	double *vb;
	double *mb;
	int64 *mib;
	uint64 *nb;
	bool dec;
	Datum pow;
	int n;
} SideRows;

enum { SCAN_BATCH = 65536 };

static void side_rows_init(TesseraState *s, SideRows *r, const TesseraSide *d, int side, tessera_dict_c *dg, bool int_v) {
	const TesseraGeneral *g = &s->g;
	r->d = d;
	r->side = side;
	r->dg = dg;
	r->n = 0;
	for (int c = 0; c < d->ng; c++) {
		Node *e = d->gexpr[c] ? (Node *)copyObject(d->gexpr[c]) : NULL;
		if (e) {
			fix_opfuncids(e);
		}
		r->gstate[c] = e ? ExecInitExpr((Expr *)e, NULL) : NULL;
	}
	for (int c = 0; c < d->nk; c++) {
		Node *e = d->kexpr[c] ? (Node *)copyObject(d->kexpr[c]) : NULL;
		if (e) {
			fix_opfuncids(e);
		}
		r->kstate[c] = e ? ExecInitExpr((Expr *)e, NULL) : NULL;
	}
	r->kb = palloc(sizeof(int64) * SCAN_BATCH);
	r->xb = palloc(sizeof(int64) * SCAN_BATCH);
	r->vb = g->has_v && !int_v ? palloc(sizeof(double) * SCAN_BATCH) : NULL;
	r->vib = g->has_v && int_v ? palloc(sizeof(int64) * SCAN_BATCH) : NULL;
	r->mb = g->nmany ? palloc(sizeof(double) * SCAN_BATCH * g->nmany) : NULL;
	r->nb = g->many_null ? palloc(sizeof(uint64) * SCAN_BATCH) : NULL;
	bool many_int = false;
	for (int m = 0; m < g->nmany; m++) {
		many_int = many_int || g->many_bits[m] > 0;
	}
	r->mib = many_int ? palloc0(sizeof(int64) * SCAN_BATCH * g->nmany) : NULL;
	r->dec = d->vatt && d->vtype == NUMERICOID && !d->vcast;
	int64 pow10 = 1;
	for (int i = 0; i < weight_scale(d); i++) {
		pow10 *= 10;
	}
	r->pow = r->dec ? NumericGetDatum(int64_to_numeric(pow10)) : 0;
}

static void side_rows_add(TesseraState *s, SideRows *r, bool int_v) {
	if (r->n == 0) {
		return;
	}
	const int rc = s->g.nmany ? tessera_c_add_many2(s->q, r->side, r->kb, r->xb, r->mb, (const int64_t *)r->mib,
	                                                (const uint64_t *)r->nb, r->n)
	               : int_v ? (r->side ? tessera_c_add_y_i64(s->q, r->kb, r->xb, (const int64_t *)r->vib, r->n)
	                                : tessera_c_add_i64(s->q, r->kb, r->xb, (const int64_t *)r->vib, r->n))
	                     : (r->side ? tessera_c_add_y(s->q, r->kb, r->xb, r->vb, r->n) : tessera_c_add(s->q, r->kb, r->xb, r->vb, r->n));
	if (rc != 0) {
		elog(ERROR, "tessera: out of memory");
	}
	r->n = 0;
}

static void side_rows_tuple(TesseraState *s, SideRows *r, TupleTableSlot *slot, ExprContext *ec, MemoryContext rowcxt,
                            StringInfo buf, bool int_v) {
	const TesseraGeneral *g = &s->g;
	const TesseraSide *d = r->d;
	const int n = r->n;
	MemoryContext old = MemoryContextSwitchTo(rowcxt);
	bool kn, xn, vn = false;
	r->kb[n] = columns_code(slot, d->nk, d->katt, d->ktype, s->dk, buf, &kn, r->kstate, ec);
	if (kn) {
		ResetExprContext(ec);
		MemoryContextSwitchTo(old);
		return;
	}
	r->xb[n] = group_code(slot, d, r->gstate, ec, r->dg, buf, &xn);
	ResetExprContext(ec);
	if (g->many_null) {
		r->nb[n] = 0;
	}
	for (int m = 0; m < g->nmany; m++) {
		if (g->many_bits[m] > 0) {
			int64 iv = 1;
			if (g->many_nf[m][r->side] == 1) {
				bool null;
				const AttrNumber at = g->many_att[m][r->side][0];
				const Datum dv = slot_getattr(slot, at, &null);
				const Oid t = TupleDescAttr(slot->tts_tupleDescriptor, at - 1)->atttypid;
				if (null) {
					if (!g->many_null) {
						elog(ERROR, "tessera: NULL group keys or values");
					}
					r->nb[n] |= (uint64)1 << m;
					iv = 0;
				} else {
					iv = t == INT2OID ? DatumGetInt16(dv) : t == INT4OID ? DatumGetInt32(dv) : DatumGetInt64(dv);
				}
			}
			r->mib[(size_t)n * g->nmany + m] = iv;
			r->mb[(size_t)n * g->nmany + m] = 0;
			continue;
		}
		double p = 1.0;
		uint64 mark = 0;
		bool is_null = false;
		for (int f = 0; f < g->many_nf[m][r->side]; f++) {
			bool null;
			const Datum dv = slot_getattr(slot, g->many_att[m][r->side][f], &null);
			if (null) {
				if (!g->many_null) {
					elog(ERROR, "tessera: NULL group keys or values");
				}
				is_null = true;
				break;
			}
			if (g->many_count[m]) {
				continue;
			}
			const double fv = DatumGetFloat8(dv), q = p * fv;
			s->many_nonfinite[m] = s->many_nonfinite[m] || !isfinite(fv);
			if (!mark && isinf(q) && !isinf(p) && !isinf(fv)) {
				mark = TESSERA_C_ROW_VALUE_OVERFLOW;
			} else if (!mark && q == 0 && p != 0 && fv != 0) {
				mark = TESSERA_C_ROW_VALUE_UNDERFLOW;
			}
			p = q;
		}
		if (isnan(p)) {
			p = (double)NAN;
		}
		if (is_null) {
			r->nb[n] |= (uint64)1 << m;
			p = g->many_count[m] ? 1.0 : 0.0;
		}
		if (mark) {
			memcpy(&p, &mark, sizeof p);
		}
		r->mb[(size_t)n * g->nmany + m] = p;
	}
	Datum vd = d->vatt ? slot_getattr(slot, d->vatt, &vn) : 0;
	const bool nan = r->dec && !vn && numeric_is_nan(DatumGetNumeric(vd));
	if (s->collect) {
		bool found;
		hash_search(s->collect, &r->kb[n], HASH_FIND, &found);
		MemoryContextSwitchTo(old);
		if (found) {
			nan_collect(s, r->kb[n], r->xb[n], nan);
		}
		return;
	}
	if (nan) {
		r->vib[n] = 0;
		MemoryContext k = MemoryContextSwitchTo(s->css.ss.ps.state->es_query_cxt);
		if (!s->nan_keys[r->side]) {
			HASHCTL ctl = {.keysize = sizeof(int64), .entrysize = sizeof(int64)};
			s->nan_keys[r->side] = hash_create("tessera NaN keys", 64, &ctl, HASH_ELEM | HASH_BLOBS);
		}
		hash_search(s->nan_keys[r->side], &r->kb[n], HASH_ENTER, NULL);
		MemoryContextSwitchTo(k);
	} else if (r->dec && !vn) {
		r->vib[n] = numeric_scaled(vd, r->pow);
	}
	double fv = 1.0;
	if (g->has_v && !int_v && d->vatt && !vn) {
		fv = datum_double(vd, d->vtype);
		s->nonfinite = s->nonfinite || !isfinite(fv);
	}
	MemoryContextSwitchTo(old);
	if (xn || vn) {
		elog(ERROR, "tessera: NULL group keys or values");
	}
	if (g->has_v && int_v) {
		r->vib[n] = r->dec ? r->vib[n] : d->vatt ? datum_int(vd, d->vtype) : 1;
	} else if (g->has_v) {
		r->vb[n] = fv;
	}
	if (++r->n == SCAN_BATCH) {
		side_rows_add(s, r, int_v);
	}
}

static void scan_rows(TesseraState *s, const TesseraSide *const *ds, const int *sides, tessera_dict_c *const *dgs, int nr,
                      Relation rel, PlanState *child, bool int_v) {
	MemoryContext rowcxt = AllocSetContextCreate(CurrentMemoryContext, "tessera scan", ALLOCSET_DEFAULT_SIZES);
	TupleTableSlot *slot = rel ? table_slot_create(rel, NULL) : NULL;
	TableScanDesc sd = rel ? table_beginscan(rel, GetActiveSnapshot(), 0, NULL) : NULL;
	ExprContext *ec = CreateExprContext(s->css.ss.ps.state);
	ExprState *qual = NULL;
	if (ds[0]->quals) {
		List *q = (List *)copyObject(ds[0]->quals);
		fix_opfuncids((Node *)q);
		qual = ExecInitQual(q, NULL);
	}
	SideRows rs[2];
	for (int i = 0; i < nr; i++) {
		side_rows_init(s, &rs[i], ds[i], sides[i], dgs[i], int_v);
	}
	StringInfoData buf;
	initStringInfo(&buf);
	int rows = 0;
	for (;;) {
		if (rel) {
			if (!table_scan_getnextslot(sd, ForwardScanDirection, slot)) {
				break;
			}
		} else {
			slot = ExecProcNode(child);
			if (TupIsNull(slot)) {
				break;
			}
		}
		CHECK_FOR_INTERRUPTS();
		if (qual) {
			ec->ecxt_scantuple = slot;
			const bool keep = ExecQual(qual, ec);
			ResetExprContext(ec);
			if (!keep) {
				continue;
			}
		}
		for (int i = 0; i < nr; i++) {
			side_rows_tuple(s, &rs[i], slot, ec, rowcxt, &buf, int_v);
		}
		if (++rows == SCAN_BATCH) {
			rows = 0;
			MemoryContextReset(rowcxt);
		}
	}
	for (int i = 0; i < nr; i++) {
		if (!s->collect) {
			side_rows_add(s, &rs[i], int_v);
		}
		pfree(rs[i].kb);
		pfree(rs[i].xb);
		if (rs[i].vb) {
			pfree(rs[i].vb);
		}
		if (rs[i].vib) {
			pfree(rs[i].vib);
		}
		if (rs[i].mb) {
			pfree(rs[i].mb);
		}
		if (rs[i].nb) {
			pfree(rs[i].nb);
		}
		if (rs[i].mib) {
			pfree(rs[i].mib);
		}
	}
	FreeExprContext(ec, true);
	if (rel) {
		table_endscan(sd);
		ExecDropSingleTupleTableSlot(slot);
	}
	pfree(buf.data);
	MemoryContextDelete(rowcxt);
}

static void scan_one(TesseraState *s, const TesseraSide *d, Relation rel, PlanState *child, int side, bool int_v,
                     tessera_dict_c *dg) {
	scan_rows(s, &d, &side, &dg, 1, rel, child, int_v);
}

typedef struct {
	int64 k, x;
	bool nan;
} NanRow;

static int nan_row_cmp(const void *a, const void *b) {
	const int64 ka = ((const NanRow *)a)->k, kb = ((const NanRow *)b)->k;
	return ka < kb ? -1 : ka > kb ? 1 : 0;
}

static void nan_cell(TesseraState *s, int64 x, int64 y) {
	int64 c[2] = {x, y};
	hash_search(s->nan_cells, c, HASH_ENTER, NULL);
}

static void nan_cells_build(TesseraState *s, bool int_v) {
	const TesseraGeneral *g = &s->g;
	MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.state->es_query_cxt);
	HASHCTL kc = {.keysize = sizeof(int64), .entrysize = sizeof(int64)};
	HTAB *keys = hash_create("tessera NaN read keys", 64, &kc, HASH_ELEM | HASH_BLOBS);
	for (int i = 0; i < 2; i++) {
		if (s->nan_keys[i]) {
			HASH_SEQ_STATUS st;
			int64 *k;
			hash_seq_init(&st, s->nan_keys[i]);
			while ((k = (int64 *)hash_seq_search(&st)) != NULL) {
				hash_search(keys, k, HASH_ENTER, NULL);
			}
		}
	}
	HASHCTL cc = {.keysize = 2 * sizeof(int64), .entrysize = 2 * sizeof(int64)};
	s->nan_cells = hash_create("tessera NaN cells", 64, &cc, HASH_ELEM | HASH_BLOBS);
	const int nsides = g->symmetric ? 1 : 2;
	NanRow *rows[2] = {NULL, NULL};
	int nrows[2] = {0, 0};
	for (int side = 0; side < nsides; side++) {
		const TesseraSide *d = &g->side[side];
		if (d->child >= 0) {
			ExecReScan((PlanState *)list_nth(s->css.custom_ps, d->child));
		}
		s->coll_n = 0;
		s->collect = keys;
		scan_side(s, d, side, int_v, side ? s->dy : s->dx);
		s->collect = NULL;
		rows[side] = palloc_extended(sizeof(NanRow) * Max(s->coll_n, 1), MCXT_ALLOC_HUGE);
		for (int i = 0; i < s->coll_n; i++) {
			rows[side][i] = (NanRow){s->coll_k[i], s->coll_x[i], s->coll_nan[i]};
		}
		nrows[side] = s->coll_n;
		qsort(rows[side], nrows[side], sizeof(NanRow), nan_row_cmp);
	}
	MemoryContextSwitchTo(old);
	const int o = g->symmetric ? 0 : 1;
	int a = 0, b = 0;
	while (a < nrows[0] && b < nrows[o]) {
		const int64 k = rows[0][a].k;
		if (rows[o][b].k < k) {
			b++;
			continue;
		}
		if (rows[o][b].k > k) {
			a++;
			continue;
		}
		int ae = a, be = b;
		while (ae < nrows[0] && rows[0][ae].k == k) {
			ae++;
		}
		while (be < nrows[o] && rows[o][be].k == k) {
			be++;
		}
		for (int i = a; i < ae; i++) {
			for (int j = b; j < be; j++) {
				if (rows[0][i].nan || rows[o][j].nan) {
					nan_cell(s, rows[0][i].x, rows[o][j].x);
					if (g->symmetric) {
						nan_cell(s, rows[o][j].x, rows[0][i].x);
					}
				}
			}
			CHECK_FOR_INTERRUPTS();
		}
		a = ae;
		b = be;
	}
}

static int pair_pass(void *ctx, int64_t a, int64_t b);

static bool same_rows(const TesseraSide *x, const TesseraSide *y) {
	if (x->child >= 0 || y->child >= 0 || x->inh || y->inh || x->relid != y->relid) {
		return false;
	}
	List *qy = (List *)copyObject(y->quals);
	ChangeVarNodes((Node *)qy, y->rti, x->rti, 0);
	return equal(qy, x->quals);
}

static void raise_float_errors(const TesseraState *s) {
	const int e = tessera_c_float_errors(s->q);
	if (e & 1) {
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: overflow")));
	}
	if (e & 2) {
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: underflow")));
	}
}

static void tessera_run_general(TesseraState *s) {
	const TesseraGeneral *g = &s->g;
	const TesseraSide *x = &g->side[0], *y = &g->side[1];
	s->q = g->symmetric ? tessera_c_new(g->has_v) : tessera_c_new_cross(g->has_v);
	if (s->q && g->nmany > 0) {
		tessera_c_set_many(s->q, (size_t)g->nmany, many_pair_op(g));
		for (int m = 0; m < g->nmany; m++) {
			if (g->many_bits[m] > 0) {
				tessera_c_set_many_int(s->q, (size_t)m, 1);
			}
		}
	}
	if (!s->q || (general_dict(x->nk, x->ktype) && !(s->dk = tessera_dict_new())) ||
	    (group_dict(x) && !(s->dx = tessera_dict_new())) || (!g->symmetric && group_dict(y) && !(s->dy = tessera_dict_new()))) {
		elog(ERROR, "tessera: out of memory");
	}
	const bool int_v = g->has_v && (!x->vatt || ((int_type(x->vtype) || x->vtype == NUMERICOID) && !x->vcast)) &&
	                   (!y->vatt || ((int_type(y->vtype) || y->vtype == NUMERICOID) && !y->vcast));
	instr_time t_read, t_run;
	INSTR_TIME_SET_CURRENT(t_read);
	if (!g->symmetric && same_rows(x, y)) {
		Relation rel = table_open(x->relid, AccessShareLock);
		if (rel->rd_rel->relkind == RELKIND_RELATION) {
			const TesseraSide *ds[2] = {x, y};
			const int sides[2] = {0, 1};
			tessera_dict_c *dgs[2] = {s->dx, s->dy};
			scan_rows(s, ds, sides, dgs, 2, rel, NULL, int_v);
		}
		table_close(rel, NoLock);
	} else {
		scan_side(s, x, 0, int_v, s->dx);
		if (!g->symmetric) {
			scan_side(s, y, 1, int_v, s->dy);
		}
	}
	INSTR_TIME_SET_CURRENT(t_run);
	INSTR_TIME_SUBTRACT(t_run, t_read);
	if (s->dx) {
		tessera_c_set_xdict(s->q, s->dx);
	}
	if (s->dy) {
		tessera_c_set_ydict(s->q, s->dy);
	}
	set_spill(s->q);
	tessera_c_set_stop(s->q, pg_should_stop, NULL);
	tessera_c_set_distinct_keys(s->q, g->distinct);
	tessera_c_set_two(s->q, g->two);
	const bool dec = (x->vatt && x->vtype == NUMERICOID) || (y->vatt && y->vtype == NUMERICOID);
	const int pb = dec ? 0 : Max(x->vatt ? int_bits(x->vtype) : 16, y->vatt ? int_bits(y->vtype) : 16);
	const bool float_v = !int_v && (g->has_v || g->nmany > 0);
	if (((int_v && pb) || float_v) && g->nfilter > 0) {
		tessera_c_set_pair_filter(s->q, pair_pass, s, float_v ? 64 : pb);
	}
	if (float_v) {
		tessera_c_set_float_errors(s->q, 1);
		tessera_c_set_float_accum(s->q, g->avgtype == FLOAT8OID);
	}
	const bool no_prune = s->nan_keys[0] || s->nan_keys[1];
	char err[256];
	instr_time t0;
	INSTR_TIME_SET_CURRENT(t0);
	if (g->group_topk > 0 && !no_prune) {
		tessera_c_set_group_top(s->q, g->group_topk, g->symmetric ? 0 : g->group_side == 0 ? 3 : 4);
	}
	int rc = tessera_c_run(s->q, 0, 1, no_prune ? 0 : (uint64_t)Max(g->minval, 0), !no_prune && g->topk > 0 ? g->topk : -1,
	                       query_threads(tessera_threads), tessera_frontier, tessera_verbose, err, sizeof err);
	if (rc != TESSERA_C_OK) {
		run_error(rc, err);
	}
	if (float_v) {
		raise_float_errors(s);
	}
	for (int m = 0; m < g->nmany; m++) {
		if (g->many_bits[m] > 0) {
			const int bits = g->many_bits[m] >= 16 ? g->many_bits[m] : 0;
			const int ov = tessera_c_many_overflow(s->q, (size_t)m, bits,
			                                       !g->many_avg[m] && ((Aggref *)list_nth(g->many_aggs, m))->aggtype == INT8OID ? 64 : 0);
			if (ov & 1) {
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				                errmsg(bits == 16 ? "smallint out of range" : bits == 32 ? "integer out of range" : "bigint out of range")));
			}
			if (ov & 2) {
				ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
			}
		}
	}
	if (tessera_verbose) {
		instr_time t1;
		INSTR_TIME_SET_CURRENT(t1);
		INSTR_TIME_SUBTRACT(t1, t0);
		elog(LOG, "tessera: general form: read %.3f s, run %.3f s", INSTR_TIME_GET_DOUBLE(t_run), INSTR_TIME_GET_DOUBLE(t1));
	}
	if (int_v) {
		const int ov = tessera_c_overflow(s->q, pb,
		                                  (g->two ? g->aggtype2 : g->aggtype) == INT8OID && g->nfilter == 0 ? 64 : 0);
		if (ov & 1) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			                errmsg(pb == 16 ? "smallint out of range" : pb == 32 ? "integer out of range" : "bigint out of range")));
		}
		if (ov & 2) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
		}
		s->wide = tessera_c_wide(s->q) != 0;
	}
	if (s->nan_keys[0] || s->nan_keys[1]) {
		nan_cells_build(s, int_v);
	}
	s->ran = true;
}

typedef struct GroupVal {
	int64 i;
	const char *p;
	uint32 len;
	bool null;
} GroupVal;

static void float_result(bool nonfinite, double v) {
	if (!nonfinite && !isfinite(v)) {
		ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("value out of range: overflow")));
	}
}

static void group_values(TesseraState *s, int side, int64 v, GroupVal *out) {
	const TesseraSide *d = &s->g.side[side];
	for (int c = 0; c < d->ng; c++) {
		out[c].null = false;
	}
	if (!group_dict(d)) {
		out[0].i = v;
		return;
	}
	size_t len;
	const char *b = side && !s->g.symmetric ? tessera_c_ystr(s->q, v, &len) : tessera_c_xstr(s->q, v, &len);
	if (!group_tuples(d)) {
		out[0].p = b;
		out[0].len = (uint32)len;
		return;
	}
	size_t q = 0;
	for (int c = 0; c < d->ng; c++) {
		if (d->gnull[c] && b[q++] == 0) {
			out[c].null = true;
			continue;
		}
		if (bytes_type(d->gtype[c])) {
			memcpy(&out[c].len, b + q, sizeof(uint32));
			q += sizeof(uint32);
			out[c].p = b + q;
			q += out[c].len;
		} else {
			memcpy(&out[c].i, b + q, sizeof(int64));
			q += sizeof(int64);
		}
	}
}

static bool filter_pass(const GroupVal *a, const GroupVal *b, Oid coll, int op) {
	if (a->null || b->null) {
		return op == 5 && a->null != b->null;
	}
	int c;
	if (OidIsValid(coll)) {
		c = varstr_cmp(a->p, (int)a->len, b->p, (int)b->len, coll);
	} else {
		c = a->i < b->i ? -1 : a->i > b->i ? 1 : 0;
	}
	return op == 0 ? c < 0 : op == 1 ? c <= 0 : op == 2 ? c > 0 : op == 3 ? c >= 0 : c != 0;
}

static int pair_pass(void *ctx, int64_t a, int64_t b) {
	TesseraState *s = (TesseraState *)ctx;
	const TesseraGeneral *g = &s->g;
	GroupVal xv[TESSERA_MAXCOLS], yv[TESSERA_MAXCOLS];
	for (int o = 0; o < (g->symmetric ? 2 : 1); o++) {
		group_values(s, 0, o ? b : a, xv);
		group_values(s, 1, o ? a : b, yv);
		bool ok = true;
		for (int f = 0; f < g->nfilter && ok; f++) {
			ok = filter_pass(&xv[g->fx[f]], &yv[g->fy[f]], g->fcoll[f], g->fop[f]);
		}
		if (ok) {
			return 1;
		}
	}
	return 0;
}

static int read_ahead_rows(void) {
	long l1 = -1;
#ifdef _SC_LEVEL1_DCACHE_SIZE
	l1 = sysconf(_SC_LEVEL1_DCACHE_SIZE);
#endif
	if (l1 <= 0) {
		l1 = 48 * 1024;
	}
	return (int)Max(l1 / (2 * 2 * 64), 1);
}

static int read_ahead(TesseraState *s) {
	if (!s->rb_ready) {
		if (!s->rb_x0) {
			MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.state->es_query_cxt);
			s->rb_cap = read_ahead_rows();
			s->rb_x0 = palloc(sizeof(int64) * s->rb_cap);
			s->rb_x1 = palloc(sizeof(int64) * s->rb_cap);
			s->rb_vi = palloc(sizeof(int64) * s->rb_cap);
			s->rb_cnt = palloc(sizeof(int64) * s->rb_cap);
			s->rb_hi = palloc(sizeof(int64) * s->rb_cap);
			s->rb_lo = palloc(sizeof(uint64) * s->rb_cap);
			s->rb_vd = palloc(sizeof(double) * s->rb_cap);
			s->rb_many = s->general && s->g.nmany ? palloc(sizeof(double) * s->rb_cap * s->g.nmany) : NULL;
			s->rb_nulls = s->general && s->g.many_null ? palloc(sizeof(uint64_t) * s->rb_cap) : NULL;
			s->rb_isums = s->general && s->g.nmany ? palloc(sizeof(int64_t) * 2 * s->rb_cap * s->g.nmany) : NULL;
			MemoryContextSwitchTo(old);
		}
		s->rb_general = tessera_c_general(s->q) != 0;
		for (int i = 0; i < 2; i++) {
			if (s->general) {
				s->rb_dict[i] = group_dict(&s->g.side[i]) ? (i && !s->g.symmetric ? 2 : 1) : 0;
			} else {
				const bool second = s->p[P_CROSS] && i == 1;
				s->rb_dict[i] = str_type((Oid)s->p[second ? P_XTYPE2 : P_XTYPE]) ? (second ? 2 : 1) : 0;
			}
		}
		s->rb_ready = true;
	}
	size_t n;
	if (s->general && s->g.nmany > 0) {
		n = tessera_c_next_many2(s->q, s->rb_x0, s->rb_x1, s->rb_cnt, s->rb_many, s->rb_isums, s->rb_nulls, (size_t)s->rb_cap);
	} else if (s->general && s->g.two) {
		n = tessera_c_next_batch2(s->q, s->rb_x0, s->rb_x1, s->rb_cnt, s->rb_hi, s->rb_lo, s->rb_vd, (size_t)s->rb_cap);
	} else if (s->wide) {
		n = tessera_c_next_batch128(s->q, s->rb_x0, s->rb_x1, s->rb_hi, s->rb_lo, (size_t)s->rb_cap);
		for (size_t i = 0; i < n; i++) {
			s->rb_vi[i] = (int64_t)s->rb_lo[i];
			s->rb_vd[i] = (double)(__int128)(((unsigned __int128)(uint64_t)s->rb_hi[i] << 64) | s->rb_lo[i]);
		}
	} else {
		n = tessera_c_next_batch(s->q, s->rb_x0, s->rb_x1, s->rb_vi, s->rb_vd, (size_t)s->rb_cap);
	}
	for (size_t i = 0; i < n; i++) {
		for (int c = 0; c < 2; c++) {
			if (s->rb_dict[c]) {
				size_t len;
				const int64 x = c ? s->rb_x1[i] : s->rb_x0[i];
				const char *b = s->rb_dict[c] == 2 ? tessera_c_ystr(s->q, x, &len) : tessera_c_xstr(s->q, x, &len);
				if (b && len) {
					__builtin_prefetch(b);
					__builtin_prefetch(b + len - 1);
				}
			}
		}
	}
	s->rb_n = (int)n;
	s->rb_pos = 0;
	return (int)n;
}

static TupleTableSlot *tessera_next_general(TesseraState *s, TupleTableSlot *slot) {
	const TesseraGeneral *g = &s->g;
	GroupVal xv[TESSERA_MAXCOLS], yv[TESSERA_MAXCOLS];
	int64_t x0, x1, vi = 0, hi = 0, cnt = 0;
	uint64_t lo = 0;
	double vd = 0;
	const double *many_row = NULL;
	const int64_t *many_irow = NULL;
	uint64_t many_nulls = 0;
	int general = 0;
	uint64 c0 = tessera_verbose ? __rdtsc() : 0;
	for (;;) {
		if (s->rb_pos == s->rb_n && read_ahead(s) == 0) {
			return slot;
		}
		const int r = s->rb_pos++;
		x0 = s->rb_x0[r];
		x1 = s->rb_x1[r];
		vd = s->rb_vd[r];
		if (g->nmany > 0) {
			cnt = s->rb_cnt[r];
			many_row = s->rb_many + (size_t)r * g->nmany;
			many_nulls = s->rb_nulls ? s->rb_nulls[r] : 0;
			many_irow = s->rb_isums + (size_t)r * g->nmany * 2;
		} else if (g->two) {
			cnt = s->rb_cnt[r];
			hi = s->rb_hi[r];
			lo = s->rb_lo[r];
		} else if (s->wide) {
			hi = s->rb_hi[r];
			lo = s->rb_lo[r];
			vi = s->rb_vi[r];
		} else {
			vi = s->rb_vi[r];
			general = s->rb_general;
		}
		const uint64 c1 = tessera_verbose ? __rdtsc() : 0;
		group_values(s, 0, x0, xv);
		group_values(s, 1, x1, yv);
		if (tessera_verbose) {
			const uint64 c2 = __rdtsc();
			s->cyc_next += c1 - c0;
			s->cyc_groups += c2 - c1;
			c0 = c2;
		}
		bool ok = true;
		for (int f = 0; f < g->nfilter && ok; f++) {
			ok = filter_pass(&xv[g->fx[f]], &yv[g->fy[f]], g->fcoll[f], g->fop[f]);
		}
		if (ok) {
			break;
		}
		CHECK_FOR_INTERRUPTS();
	}
	if (g->nfilter > 0 && (g->two ? g->aggtype2 : g->aggtype) == INT8OID && (g->two || s->wide)) {
		const __int128 v = (__int128)(((unsigned __int128)(uint64_t)hi << 64) | lo);
		if (v < PG_INT64_MIN || v > PG_INT64_MAX) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
		}
	}
	const int vs = weight_scale(&g->side[0]) + weight_scale(&g->side[1]);
	bool nan = false;
	if (s->nan_cells) {
		int64 cell[2] = {x0, x1};
		hash_search(s->nan_cells, cell, HASH_FIND, &nan);
	}
	Datum nan_d = (Datum)0;
	if (nan) {
		MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
		nan_d = DirectFunctionCall3(numeric_in, CStringGetDatum("NaN"), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1));
		MemoryContextSwitchTo(old);
	}
	for (int c = 0; c < s->ncol; c++) {
		const int k = s->kind[c];
		Datum d;
		if (k >= 300 && g->many_count[k - 300]) {
			d = Int64GetDatum(many_nulls >> (k - 300) & 1 ? 0 : (int64)many_row[k - 300]);
		} else if (k >= 300 && many_nulls >> (k - 300) & 1) {
			slot->tts_values[c] = (Datum)0;
			slot->tts_isnull[c] = true;
			continue;
		} else if (k >= 300 && g->many_avg[k - 300]) {
			const int m = k - 300;
			MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
			d = DirectFunctionCall2(numeric_div, numeric_of_i128(many_irow[2 * m + 1], (uint64_t)many_irow[2 * m]),
			                        NumericGetDatum(int64_to_numeric((int64)many_row[m + 1])));
			MemoryContextSwitchTo(old);
		} else if (k >= 300 && g->many_bits[k - 300] > 0) {
			const int64_t lo = many_irow[2 * (k - 300)], hi = many_irow[2 * (k - 300) + 1];
			MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
			d = ((Aggref *)list_nth(g->many_aggs, k - 300))->aggtype == INT8OID ? Int64GetDatum(lo)
			                                                                   : numeric_of_i128(hi, (uint64_t)lo);
			MemoryContextSwitchTo(old);
		} else if (k >= 300) {
			float_result(s->many_nonfinite[k - 300], many_row[k - 300]);
			d = Float8GetDatum(many_row[k - 300]);
		} else if (k == 2 && g->nmany > 0) {
			d = Int64GetDatum(cnt);
		} else if (nan && ((k == 4 && g->avgtype == NUMERICOID) || (k == 3 && g->aggtype2 == NUMERICOID) ||
		            (k == 2 && !g->two && g->aggtype == NUMERICOID))) {
			d = nan_d;
		} else if (k == 2 && g->two) {
			d = Int64GetDatum(cnt);
		} else if (k == 4) {
			MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
			if (g->avgtype != NUMERICOID) {
				float_result(s->nonfinite, vd);
			}
			d = g->avgtype == NUMERICOID ? DirectFunctionCall2(numeric_div, numeric_of_i128_scaled(hi, lo, vs),
			                                                   NumericGetDatum(int64_to_numeric(cnt)))
			                             : Float8GetDatum(vd / (double)cnt);
			MemoryContextSwitchTo(old);
		} else if (k == 3) {
			if (g->aggtype2 == NUMERICOID) {
				MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
				d = numeric_of_i128_scaled(hi, lo, vs);
				MemoryContextSwitchTo(old);
			} else {
				if (g->aggtype2 == FLOAT8OID) {
					float_result(s->nonfinite, vd);
				}
				d = g->aggtype2 == FLOAT8OID ? Float8GetDatum(vd) : Int64GetDatum((int64)lo);
			}
		} else if (k == 2) {
			if (g->aggtype == NUMERICOID) {
				MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
				d = s->wide ? numeric_of_i128_scaled(hi, lo, vs) : numeric_of_i128_scaled(vi < 0 ? -1 : 0, (uint64_t)vi, vs);
				MemoryContextSwitchTo(old);
			} else {
				if (g->aggtype == FLOAT8OID) {
					float_result(s->nonfinite, general || s->wide ? vd : (double)vi);
				}
				d = g->aggtype == FLOAT8OID ? Float8GetDatum(general || s->wide ? vd : (double)vi) : Int64GetDatum(vi);
			}
		} else {
			const int side = k >= 200, i = k - (side ? 200 : 100);
			const Oid t = g->side[side].gtype[i];
			const GroupVal *v = side ? &yv[i] : &xv[i];
			if (v->null) {
				slot->tts_values[c] = (Datum)0;
				slot->tts_isnull[c] = true;
				continue;
			}
			if (str_type(t)) {
				MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
				d = PointerGetDatum(cstring_to_text_with_len(v->p, (int)v->len));
				MemoryContextSwitchTo(old);
			} else if (text_type(t)) {
				MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
				Oid in, ioparam;
				getTypeInputInfo(t, &in, &ioparam);
				d = OidInputFunctionCall(in, pnstrdup(v->p, v->len), ioparam, -1);
				MemoryContextSwitchTo(old);
			} else {
				d = int_datum(v->i, t);
			}
		}
		slot->tts_values[c] = d;
		slot->tts_isnull[c] = false;
	}
	if (tessera_verbose) {
		s->cyc_cols += __rdtsc() - c0;
		s->trace_rows++;
	}
	return ExecStoreVirtualTuple(slot);
}

typedef struct DictSort {
	tessera_dict_c *d;
	Oid coll;
} DictSort;

static int dict_cmp(const void *a, const void *b, void *arg) {
	DictSort *ds = (DictSort *)arg;
	size_t la, lb;
	const char *sa = tessera_dict_str(ds->d, *(const int64 *)a, &la), *sb = tessera_dict_str(ds->d, *(const int64 *)b, &lb);
	return varstr_cmp(sa, (int)la, sb, (int)lb, ds->coll);
}

static void order_dict(tessera_dict_c *d, Oid coll) {
	const size_t n = tessera_dict_size(d);
	int64 *codes = palloc(sizeof(int64) * Max(n, 1)), *rank = palloc(sizeof(int64) * Max(n, 1));
	for (size_t c = 0; c < n; c++) {
		codes[c] = (int64)c;
	}
	DictSort ds = {d, coll};
	qsort_arg(codes, n, sizeof(int64), dict_cmp, &ds);
	for (size_t i = 0; i < n; i++) {
		rank[codes[i]] = (int64)i;
	}
	if (tessera_dict_set_order(d, (const int64_t *)rank, n) != 0) {
		elog(ERROR, "tessera: the collation's order of the group keys is not a permutation");
	}
	pfree(codes);
	pfree(rank);
}

static void tessera_run(TesseraState *s) {
	if (s->general) {
		tessera_run_general(s);
		return;
	}
	const bool has_v = s->p[P_VATT] != 0, cross = s->p[P_CROSS] != 0;
	const Oid kt = (Oid)s->p[P_KTYPE], xt = (Oid)s->p[P_XTYPE], yt = (Oid)s->p[P_XTYPE2];
	s->q = cross ? tessera_c_new_cross(has_v) : tessera_c_new(has_v);
	if (!s->q || (str_type(kt) && !(s->dk = tessera_dict_new())) || (str_type(xt) && !(s->dx = tessera_dict_new())) ||
	    (cross && str_type(yt) && !(s->dy = tessera_dict_new()))) {
		elog(ERROR, "tessera: out of memory");
	}
	const bool int_v = has_v && int_type((Oid)s->p[P_VTYPE]) && (!cross || int_type((Oid)s->p[P_VTYPE2]));
	scan_rel(s->q, s->dk, s->dx, (Oid)s->p[P_RELID], (AttrNumber)s->p[P_KATT], (AttrNumber)s->p[P_XATT],
	         (AttrNumber)s->p[P_VATT], kt, xt, (Oid)s->p[P_VTYPE], 0, int_v, &s->nonfinite);
	if (cross) {
		scan_rel(s->q, s->dk, s->dy, (Oid)s->p[P_RELID2], (AttrNumber)s->p[P_KATT2], (AttrNumber)s->p[P_XATT2],
		         (AttrNumber)s->p[P_VATT2], (Oid)s->p[P_KTYPE2], yt, (Oid)s->p[P_VTYPE2], 1, int_v, &s->nonfinite);
	}
	if (s->dx && s->p[P_XCOLL]) {
		order_dict(s->dx, (Oid)s->p[P_XCOLL]);
	}
	if (s->dx) {
		tessera_c_set_xdict(s->q, s->dx);
	}
	if (s->dy) {
		tessera_c_set_ydict(s->q, s->dy);
	}
	set_spill(s->q);
	tessera_c_set_stop(s->q, pg_should_stop, NULL);
	tessera_c_set_distinct_keys(s->q, s->p[P_DISTINCT] != 0);
	char err[256];
	if (s->p[P_GROUP_TOPK] > 0) {
		tessera_c_set_group_top(s->q, s->p[P_GROUP_TOPK], (int)s->p[P_GROUP_OF]);
	}
	if (has_v && !int_v) {
		tessera_c_set_float_errors(s->q, 1);
	}
	int rc = tessera_c_run(s->q, (int)s->p[P_MODE], (int)s->p[P_COL0_SMALLER], (uint64_t)s->p[P_MINVAL],
	                       s->p[P_TOPK] > 0 ? s->p[P_TOPK] : -1,
	                       query_threads(tessera_threads), tessera_frontier, tessera_verbose, err, sizeof err);
	if (rc != TESSERA_C_OK) {
		run_error(rc, err);
	}
	if (has_v && !int_v) {
		raise_float_errors(s);
	}
	if (int_v) {
		const int pb = Max(int_bits((Oid)s->p[P_VTYPE]), int_bits(cross ? (Oid)s->p[P_VTYPE2] : (Oid)s->p[P_VTYPE]));
		const int ov = tessera_c_overflow(s->q, pb, (Oid)s->p[P_AGGTYPE] == INT8OID ? 64 : 0);
		if (ov & 1) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
			                errmsg(pb == 16 ? "smallint out of range" : pb == 32 ? "integer out of range" : "bigint out of range")));
		}
		if (ov & 2) {
			ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("bigint out of range")));
		}
		s->wide = tessera_c_wide(s->q) != 0;
	}
	s->ran = true;
}

static Datum key_datum(TesseraState *s, int64 x, Oid t, int side) {
	if (str_type(t)) {
		size_t len;
		const char *p = side ? tessera_c_ystr(s->q, x, &len) : tessera_c_xstr(s->q, x, &len);
		MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
		Datum d = PointerGetDatum(cstring_to_text_with_len(p, (int)len));
		MemoryContextSwitchTo(old);
		return d;
	}
	return int_datum(x, t);
}

static TupleTableSlot *tessera_next(ScanState *ss) {
	TesseraState *s = (TesseraState *)ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;
	ExecClearTuple(slot);
	if (!s->ran) {
		tessera_run(s);
	}
	if (s->general) {
		return tessera_next_general(s, slot);
	}
	const uint64 c0 = tessera_verbose ? __rdtsc() : 0;
	int64_t x0, x1, vi, hi = 0;
	uint64_t lo = 0;
	double vd = 0;
	int general = 0;
	if (s->rb_pos == s->rb_n && read_ahead(s) == 0) {
		return slot;
	}
	const int r = s->rb_pos++;
	x0 = s->rb_x0[r];
	x1 = s->rb_x1[r];
	vi = s->rb_vi[r];
	vd = s->rb_vd[r];
	if (s->wide) {
		hi = s->rb_hi[r];
		lo = s->rb_lo[r];
	} else {
		general = s->rb_general;
	}
	const uint64 c1 = tessera_verbose ? __rdtsc() : 0;
	const Oid xt = (Oid)s->p[P_XTYPE], at = (Oid)s->p[P_AGGTYPE];
	for (int c = 0; c < s->ncol; c++) {
		Datum d;
		if (s->kind[c] == 2 && at == NUMERICOID) {
			MemoryContext old = MemoryContextSwitchTo(s->css.ss.ps.ps_ExprContext->ecxt_per_tuple_memory);
			d = s->wide ? numeric_of_i128(hi, lo) : NumericGetDatum(int64_to_numeric(vi));
			MemoryContextSwitchTo(old);
		} else if (s->kind[c] == 2) {
			if (at == FLOAT8OID) {
				float_result(s->nonfinite, general || s->wide ? vd : (double)vi);
			}
			d = at == FLOAT8OID ? Float8GetDatum(general || s->wide ? vd : (double)vi) : Int64GetDatum(vi);
		} else {
			const int side = s->p[P_CROSS] && s->kind[c] == 1;
			d = key_datum(s, s->kind[c] == 0 ? x0 : x1, side ? (Oid)s->p[P_XTYPE2] : xt, side);
		}
		slot->tts_values[c] = d;
		slot->tts_isnull[c] = false;
	}
	if (tessera_verbose) {
		s->cyc_next += c1 - c0;
		s->cyc_cols += __rdtsc() - c1;
		s->trace_rows++;
	}
	return ExecStoreVirtualTuple(slot);
}

static bool tessera_recheck(ScanState *ss, TupleTableSlot *slot) {
	return true;
}

static TupleTableSlot *tessera_exec(CustomScanState *node) {
	return ExecScan(&node->ss, (ExecScanAccessMtd)tessera_next, (ExecScanRecheckMtd)tessera_recheck);
}

static void tessera_end(CustomScanState *node) {
	TesseraState *ts = (TesseraState *)node;
	if (tessera_verbose && ts->trace_rows) {
		elog(LOG, "tessera: %s: emitted %llu rows, cycles per row: next %.0f, groups %.0f, columns %.0f",
		     ts->general ? "general form" : "classic form",
		     (unsigned long long)ts->trace_rows, (double)ts->cyc_next / ts->trace_rows, (double)ts->cyc_groups / ts->trace_rows,
		     (double)ts->cyc_cols / ts->trace_rows);
	}
	ListCell *lc;
	foreach (lc, node->custom_ps) {
		ExecEndNode((PlanState *)lfirst(lc));
	}
	tessera_release(node);
}

static void tessera_rescan(CustomScanState *node) {
	TesseraState *s = (TesseraState *)node;
	bool again = false;
	ListCell *lc;
	foreach (lc, node->custom_ps) {
		PlanState *c = (PlanState *)lfirst(lc);
		if (node->ss.ps.chgParam != NULL) {
			UpdateChangedParamSet(c, node->ss.ps.chgParam);
		}
		again = again || c->chgParam != NULL;
	}
	s->rb_n = s->rb_pos = 0;
	if (!again) {
		if (s->q) {
			tessera_c_rewind(s->q);
		}
		return;
	}
	foreach (lc, node->custom_ps) {
		PlanState *c = (PlanState *)lfirst(lc);
		if (c->chgParam == NULL) {
			ExecReScan(c);
		}
	}
	tessera_release(s);
	s->ran = false;
	s->rb_ready = false;
}

static void tessera_explain(CustomScanState *node, List *ancestors, ExplainState *es) {
	TesseraState *s = (TesseraState *)node;
	static const char *modes[] = {"all ordered pairs", "x_a < x_b", "x_a <= x_b", "x_a <> x_b"};
	if (s->general) {
		const TesseraGeneral *g = &s->g;
		ExplainPropertyText("Tessera pairs", g->symmetric ? "self-join, both orders" : "cross product of two inputs", es);
		if (g->two) {
			bool has[5] = {false, false, false, false, false};
			for (int c = 0; c < s->ncol; c++) {
				if (s->kind[c] >= 2 && s->kind[c] <= 4) {
					has[s->kind[c]] = true;
				}
			}
			char agg[96] = "";
			if (has[2]) {
				strlcat(agg, g->distinct ? "COUNT(DISTINCT a.k)" : "COUNT(*)", sizeof agg);
			}
			if (has[3]) {
				strlcat(agg, has[2] ? ", SUM(a.v * b.w)" : "SUM(a.v * b.w)", sizeof agg);
			}
			if (has[4]) {
				strlcat(agg, has[2] || has[3] ? ", AVG(a.v * b.w)" : "AVG(a.v * b.w)", sizeof agg);
			}
			ExplainPropertyText("Tessera aggregate", agg, es);
		} else {
			ExplainPropertyText("Tessera aggregate",
			                    g->has_v ? "SUM(a.v * b.w)" : g->distinct ? "COUNT(DISTINCT a.k)" : "COUNT(*)", es);
		}
		ExplainPropertyInteger("Tessera key columns", NULL, g->side[0].nk, es);
		ExplainPropertyInteger("Tessera group columns", NULL, g->side[0].ng + g->side[1].ng, es);
		ExplainPropertyInteger("Tessera input filters", NULL, list_length(g->side[0].quals) + list_length(g->side[1].quals), es);
		if (g->nfilter > 0) {
			ExplainPropertyInteger("Tessera row filters", NULL, g->nfilter, es);
		}
		if (g->minval > 0) {
			ExplainPropertyInteger("Tessera HAVING prune below", NULL, g->minval, es);
		}
		if (g->topk > 0) {
			ExplainPropertyInteger("Tessera top-k prune", NULL, g->topk, es);
		}
		if (g->group_topk > 0) {
			ExplainPropertyInteger("Tessera rank prune", NULL, g->group_topk, es);
		}
		ExplainPropertyText("Tessera core", psprintf("%s (%s)", tessera_c_version(), tessera_c_isa()), es);
		return;
	}
	ExplainPropertyText("Tessera pairs", s->p[P_CROSS] ? "cross product of two inputs" : modes[s->p[P_MODE] % 4], es);
	ExplainPropertyText("Tessera aggregate",
	                    s->p[P_VATT] ? "SUM(a.v * b.v)" : s->p[P_DISTINCT] ? "COUNT(DISTINCT a.k)" : "COUNT(*)", es);
	if (s->p[P_MINVAL] > 0) {
		ExplainPropertyInteger("Tessera HAVING prune below", NULL, s->p[P_MINVAL], es);
	}
	if (s->p[P_TOPK] > 0) {
		ExplainPropertyInteger("Tessera top-k prune", NULL, s->p[P_TOPK], es);
	}
	if (s->p[P_GROUP_TOPK] > 0) {
		ExplainPropertyInteger("Tessera rank prune", NULL, s->p[P_GROUP_TOPK], es);
	}
	ExplainPropertyText("Tessera core", psprintf("%s (%s)", tessera_c_version(), tessera_c_isa()), es);
}

PG_FUNCTION_INFO_V1(tessera_sim);
PG_FUNCTION_INFO_V1(tessera_sim_text);

static AttrNumber sim_column(Oid relid, int arg, FunctionCallInfo fcinfo, Oid *type, Oid *coll) {
	if (PG_ARGISNULL(arg)) {
		elog(ERROR, "tessera_sim: argument %d is NULL", arg + 1);
	}
	char *name = text_to_cstring(PG_GETARG_TEXT_PP(arg));
	AttrNumber att = get_attnum(relid, name);
	if (att <= 0) {
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN),
		                errmsg("tessera_sim: column \"%s\" of relation \"%s\" does not exist", name, get_rel_name(relid))));
	}
	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
	    pg_attribute_aclcheck(relid, att, GetUserId(), ACL_SELECT) != ACLCHECK_OK) {
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(get_rel_relkind(relid)), get_rel_name(relid));
	}
	if (check_enable_rls(relid, InvalidOid, true) != RLS_NONE) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("tessera_sim: relation \"%s\" has row-level security, which tessera_sim does not apply",
		                       get_rel_name(relid))));
	}
	if (get_attgenerated(relid, att) == ATTRIBUTE_GENERATED_VIRTUAL) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("tessera_sim: column \"%s\" is a virtual generated column, which has no stored values", name)));
	}
	Oid typmod_coll;
	int32 typmod;
	get_atttypetypmodcoll(relid, att, type, &typmod, &typmod_coll);
	*coll = typmod_coll;
	return att;
}

static Datum sim_run(FunctionCallInfo fcinfo, bool text_items) {
	const char *fn = text_items ? "tessera_sim_text" : "tessera_sim";
	for (int a = 0; a < 5; a++) {
		if (PG_ARGISNULL(a)) {
			elog(ERROR, "%s: argument %d is NULL", fn, a + 1);
		}
	}
	if (!tessera_c_supported()) {
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		                errmsg("%s: this CPU lacks x86-64-v3 (AVX2, BMI2, FMA), which Tessera needs", fn)));
	}
	const Oid relid = PG_GETARG_OID(0);
	LockRelationOid(relid, AccessShareLock);
	const char relkind = get_rel_relkind(relid);
	if ((relkind != RELKIND_RELATION && relkind != RELKIND_MATVIEW) || has_subclass(relid)) {
		elog(ERROR, "%s: \"%s\" is not a table without inheritance children", fn, get_rel_name(relid));
	}
	if (relkind == RELKIND_MATVIEW) {
		Relation r = table_open(relid, NoLock);
		const bool populated = RelationIsPopulated(r);
		table_close(r, NoLock);
		if (!populated) {
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			                errmsg("materialized view \"%s\" has not been populated", get_rel_name(relid)),
			                errhint("Use the REFRESH MATERIALIZED VIEW command.")));
		}
	}
	Oid kt, xt, vt = InvalidOid, kc, xc, vc;
	const AttrNumber ka = sim_column(relid, 1, fcinfo, &kt, &kc), xa = sim_column(relid, 2, fcinfo, &xt, &xc);
	const AttrNumber va = PG_ARGISNULL(5) ? 0 : sim_column(relid, 5, fcinfo, &vt, &vc);
	if (!key_ok(kt, kc)) {
		elog(ERROR, "%s: k must be an integer, or text or varchar with a deterministic collation", fn);
	}
	if (text_items ? !(str_type(xt) && key_ok(xt, xc)) : !int_type(xt)) {
		if (text_items) {
			elog(ERROR, "%s: x must be text or varchar with a deterministic collation (tessera_sim takes integers)", fn);
		}
		elog(ERROR, "%s: x must be an integer (tessera_sim_text takes strings)", fn);
	}
	if (va && !(int_type(vt) || vt == FLOAT4OID || vt == FLOAT8OID)) {
		elog(ERROR, "%s: v must be an integer or a float", fn);
	}
	const char *ms = text_to_cstring(PG_GETARG_TEXT_PP(3));
	const int measure = !strcmp(ms, "cosine") ? 1 : !strcmp(ms, "jaccard") ? 2 : !strcmp(ms, "dice") ? 3
	                    : !strcmp(ms, "overlap") ? 4 : 0;
	if (!measure) {
		elog(ERROR, "%s: measure is 'cosine', 'jaccard', 'dice' or 'overlap'", fn);
	}
	const char *md = PG_ARGISNULL(6) ? "lt" : text_to_cstring(PG_GETARG_TEXT_PP(6));
	const int mode = !strcmp(md, "full") ? 0 : !strcmp(md, "lt") ? 1 : !strcmp(md, "le") ? 2 : !strcmp(md, "ne") ? 3 : -1;
	if (mode < 0) {
		elog(ERROR, "%s: mode is 'lt', 'le', 'full' or 'ne'", fn);
	}
	const int threads = query_threads(!PG_ARGISNULL(7) && PG_GETARG_INT32(7) > 0 ? PG_GETARG_INT32(7) : tessera_threads);
	InitMaterializedSRF(fcinfo, 0);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *)fcinfo->resultinfo;
	tessera_query_c *q = tessera_c_new(va != 0);
	tessera_dict_c *volatile dk = NULL, *volatile dx = NULL;
	PG_TRY();
	{
		if (!q || (str_type(kt) && !(dk = tessera_dict_new())) || (text_items && !(dx = tessera_dict_new()))) {
			elog(ERROR, "%s: out of memory", fn);
		}
		scan_rel(q, dk, dx, relid, ka, xa, va, kt, xt, vt, 0, false, NULL);
		if (dx) {
			tessera_c_set_xdict(q, dx);
		}
		tessera_c_set_similarity(q, measure, PG_GETARG_FLOAT8(4));
		set_spill(q);
		tessera_c_set_stop(q, pg_should_stop, NULL);
		char err[256];
		const int rc = tessera_c_run(q, mode, 1, 0, -1, threads, tessera_frontier, tessera_verbose, err, sizeof err);
		if (rc != TESSERA_C_OK) {
			run_error(rc, err);
		}
		int64_t x0, x1, vi;
		double vd;
		int general;
		Datum values[3];
		bool nulls[3] = {false, false, false};
		MemoryContext rowcxt = AllocSetContextCreate(CurrentMemoryContext, "tessera_sim rows", ALLOCSET_DEFAULT_SIZES);
		while (tessera_c_next(q, &x0, &x1, &vi, &vd, &general)) {
			MemoryContext old = MemoryContextSwitchTo(rowcxt);
			if (text_items) {
				size_t l0, l1;
				const char *p0 = tessera_c_xstr(q, x0, &l0), *p1 = tessera_c_xstr(q, x1, &l1);
				values[0] = PointerGetDatum(cstring_to_text_with_len(p0, (int)l0));
				values[1] = PointerGetDatum(cstring_to_text_with_len(p1, (int)l1));
			} else {
				values[0] = Int64GetDatum(x0);
				values[1] = Int64GetDatum(x1);
			}
			values[2] = Float8GetDatum(vd);
			tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
			MemoryContextSwitchTo(old);
			MemoryContextReset(rowcxt);
			CHECK_FOR_INTERRUPTS();
		}
		MemoryContextDelete(rowcxt);
	}
	PG_FINALLY();
	{
		if (q) {
			tessera_c_free(q);
		}
		if (dk) {
			tessera_dict_free(dk);
		}
		if (dx) {
			tessera_dict_free(dx);
		}
	}
	PG_END_TRY();
	return (Datum)0;
}

Datum tessera_sim(PG_FUNCTION_ARGS) {
	return sim_run(fcinfo, false);
}

Datum tessera_sim_text(PG_FUNCTION_ARGS) {
	return sim_run(fcinfo, true);
}

void _PG_init(void) {
	if (tessera_c_api_version() != TESSERA_API_VERSION) {
		elog(ERROR, "tessera: the core library has interface version %d, this module needs %d", tessera_c_api_version(),
		     TESSERA_API_VERSION);
	}
	DefineCustomBoolVariable("tessera.enabled", "Plan self-join aggregates with the Tessera operator", NULL,
	                         &tessera_enabled, true, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("tessera.threads", "Threads of the Tessera operator (0: max_parallel_workers_per_gather + 1)",
	                        NULL, &tessera_threads, 0, 0, 1024,
	                        PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomIntVariable("tessera.frontier", "Items on the matrix engine (-1: cost model)", NULL, &tessera_frontier,
	                        -1, -1, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomBoolVariable("tessera.trace", "Tessera phase times to the server log (stderr)", NULL, &tessera_verbose,
	                         false, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("tessera.min_expansion",
	                         "PostgreSQL's plan keeps self-joins with fewer estimated joined rows per input row (0: never)",
	                         NULL, &tessera_min_expansion, 0, 0, DBL_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	DefineCustomRealVariable("tessera.min_expansion_cross", "The same limit for joins of two relations (0: never)",
	                         NULL, &tessera_min_expansion_cross, 0, 0, DBL_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
	MarkGUCPrefixReserved("tessera");
	RegisterCustomScanMethods(&tessera_scan_methods);
	prev_upper_hook = create_upper_paths_hook;
	create_upper_paths_hook = tessera_upper_hook;
}
