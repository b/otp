/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 1996-2010. All Rights Reserved.
 *
 * The contents of this file are subject to the Erlang Public License,
 * Version 1.1, (the "License"); you may not use this file except in
 * compliance with the License. You should have received a copy of the
 * Erlang Public License along with this software. If not, it can be
 * retrieved online at http://www.erlang.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * %CopyrightEnd%
 */

/*
 * This file contains the bif interface functions and
 * the handling of the "meta tables" ie the tables of 
 * db tables.
 */

/*
#ifdef DEBUG
#define HARDDEBUG 1
#endif
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "erl_process.h"
#include "error.h"
#define ERTS_WANT_DB_INTERNAL__
#include "erl_db.h"
#include "bif.h"
#include "big.h"


erts_smp_atomic_t erts_ets_misc_mem_size;

/*
** Utility macros
*/

/* Get a key from any table structure and a tagged object */
#define TERM_GETKEY(tb, obj) db_getkey((tb)->common.keypos, (obj)) 

 
/* How safe are we from double-hits or missed objects
** when iterating without fixation? */ 
enum DbIterSafety {
    ITER_UNSAFE,      /* Must fixate to be safe */
    ITER_SAFE_LOCKED, /* Safe while table is locked, not between trap calls */
    ITER_SAFE         /* No need to fixate at all */
};
#ifdef ERTS_SMP
#  define ITERATION_SAFETY(Proc,Tab) \
    ((IS_TREE_TABLE((Tab)->common.status) || ONLY_WRITER(Proc,Tab)) ? ITER_SAFE \
     : (((Tab)->common.status & DB_FINE_LOCKED) ? ITER_UNSAFE : ITER_SAFE_LOCKED))
#else
#  define ITERATION_SAFETY(Proc,Tab) \
    ((IS_TREE_TABLE((Tab)->common.status) || ONLY_WRITER(Proc,Tab)) \
     ? ITER_SAFE : ITER_SAFE_LOCKED)
#endif

#define DID_TRAP(P,Ret) (!is_value(Ret) && ((P)->freason == TRAP))


/* 
** The main meta table, containing all ets tables.
*/
#ifdef ERTS_SMP

#define ERTS_META_MAIN_TAB_LOCK_TAB_BITS 8
#define ERTS_META_MAIN_TAB_LOCK_TAB_SIZE (1 << ERTS_META_MAIN_TAB_LOCK_TAB_BITS)
#define ERTS_META_MAIN_TAB_LOCK_TAB_MASK (ERTS_META_MAIN_TAB_LOCK_TAB_SIZE - 1)

typedef union {
    erts_smp_rwmtx_t rwmtx;
    byte cache_line_align__[ERTS_ALC_CACHE_LINE_ALIGN_SIZE(
				sizeof(erts_smp_rwmtx_t))];
} erts_meta_main_tab_lock_t;

static erts_meta_main_tab_lock_t *meta_main_tab_locks;

#endif
static struct {
    union {
	DbTable *tb;     /* Only directly readable if slot is ALIVE */
	UWord next_free;  /* (index<<2)|1 if slot is FREE */
    }u;
} *meta_main_tab;

/* A slot in meta_main_tab can have three states:
 * FREE : Free to use for new table. Part of linked free-list.
 * ALIVE: Contains a table
 * DEAD : Contains a table that is being removed.
 */
#define IS_SLOT_FREE(i)	(meta_main_tab[(i)].u.next_free & 1)
#define IS_SLOT_DEAD(i) (meta_main_tab[(i)].u.next_free & 2)
#define IS_SLOT_ALIVE(i) (!(meta_main_tab[(i)].u.next_free & (1|2)))
#define GET_NEXT_FREE_SLOT(i) (meta_main_tab[(i)].u.next_free >> 2)
#define SET_NEXT_FREE_SLOT(i,next) (meta_main_tab[(i)].u.next_free = ((next)<<2)|1)
#define MARK_SLOT_DEAD(i) (meta_main_tab[(i)].u.next_free |= 2)
#define GET_ANY_SLOT_TAB(i) ((DbTable*)(meta_main_tab[(i)].u.next_free & ~(1|2))) /* dead or alive */

static ERTS_INLINE erts_smp_rwmtx_t *
get_meta_main_tab_lock(unsigned slot)
{
#ifdef ERTS_SMP
    return &meta_main_tab_locks[slot & ERTS_META_MAIN_TAB_LOCK_TAB_MASK].rwmtx;
#else
    return NULL;
#endif
}

static erts_smp_spinlock_t meta_main_tab_main_lock;
static Uint meta_main_tab_first_free;   /* Index of first free slot */
static int meta_main_tab_cnt;		/* Number of active tables */
static Uint meta_main_tab_slot_mask;    /* The slot index part of an unnamed table id */
static Uint meta_main_tab_seq_incr;
static Uint meta_main_tab_seq_cnt = 0;  /* To give unique(-ish) table identifiers */



/* 
** The meta hash table of all NAMED ets tables
*/
#ifdef ERTS_SMP
#  define META_NAME_TAB_LOCK_CNT 16
union {
    erts_smp_rwmtx_t lck;
    byte _cache_line_alignment[64];
}meta_name_tab_rwlocks[META_NAME_TAB_LOCK_CNT];
#endif
static struct meta_name_tab_entry {
    union {
	Eterm name_atom;
	Eterm mcnt; /* Length of mvec in multiple tab entry */
    }u;
    union {
	DbTable *tb;
	struct meta_name_tab_entry* mvec;
    }pu;
} *meta_name_tab;

static unsigned meta_name_tab_mask;

static ERTS_INLINE
struct meta_name_tab_entry* meta_name_tab_bucket(Eterm name, 
						 erts_smp_rwmtx_t** lockp)
{
    unsigned bix = atom_val(name) & meta_name_tab_mask;
    struct meta_name_tab_entry* bucket = &meta_name_tab[bix];
#ifdef ERTS_SMP
    *lockp = &meta_name_tab_rwlocks[bix % META_NAME_TAB_LOCK_CNT].lck;
#endif
    return bucket;
}    


typedef enum {
    LCK_READ=1,     /* read only access */
    LCK_WRITE=2,    /* exclusive table write access */
    LCK_WRITE_REC=3, /* record write access */
    LCK_NONE=4
} db_lock_kind_t;

extern DbTableMethod db_hash;
extern DbTableMethod db_tree;

int user_requested_db_max_tabs;
int erts_ets_realloc_always_moves;
static int db_max_tabs;
static DbTable *meta_pid_to_tab; /* Pid mapped to owned tables */
static DbTable *meta_pid_to_fixed_tab; /* Pid mapped to fixed tables */
static Eterm ms_delete_all;
static Eterm ms_delete_all_buff[8]; /* To compare with for deletion 
				       of all objects */

/* 
** Forward decls, static functions 
*/

static void fix_table_locked(Process* p, DbTable* tb);
static void unfix_table_locked(Process* p,  DbTable* tb, db_lock_kind_t* kind);
static void set_heir(Process* me, DbTable* tb, Eterm heir, UWord heir_data);
static void free_heir_data(DbTable*);
static void free_fixations_locked(DbTable *tb);

static int free_table_cont(Process *p,
			   DbTable *tb,
			   int first,
			   int clean_meta_tab);
static void print_table(int to, void *to_arg, int show,  DbTable* tb);
static BIF_RETTYPE ets_select_delete_1(Process *p, Eterm a1);
static BIF_RETTYPE ets_select_count_1(Process *p, Eterm a1);
static BIF_RETTYPE ets_select_trap_1(Process *p, Eterm a1);
static BIF_RETTYPE ets_delete_trap(Process *p, Eterm a1);
static Eterm table_info(Process* p, DbTable* tb, Eterm What);

/* 
 * Exported global
 */
Export ets_select_delete_continue_exp;
Export ets_select_count_continue_exp;
Export ets_select_continue_exp;

/*
 * Static traps
 */
static Export ets_delete_continue_exp;

static ERTS_INLINE DbTable* db_ref(DbTable* tb, db_lock_kind_t kind)
{
    if (tb != NULL && kind != LCK_READ) {
	erts_refc_inc(&tb->common.ref, 2);
    }
    return tb;
}

static ERTS_INLINE DbTable* db_unref(DbTable* tb, db_lock_kind_t kind)
{
    if (kind != LCK_READ && !erts_refc_dectest(&tb->common.ref, 0)) {
#ifdef HARDDEBUG
	if (erts_smp_atomic_read(&tb->common.memory_size) != sizeof(DbTable)) {
	    erts_fprintf(stderr, "ets: db_unref memory remain=%ld fix=%x\n",
			 erts_smp_atomic_read(&tb->common.memory_size)-sizeof(DbTable), 
			 tb->common.fixations);
	}
	erts_fprintf(stderr, "ets: db_unref(%T) deleted!!!\r\n", 
		     tb->common.id);

	erts_fprintf(stderr, "ets: db_unref: meta_pid_to_tab common.memory_size = %ld\n",
		     erts_smp_atomic_read(&meta_pid_to_tab->common.memory_size));
	print_table(ERTS_PRINT_STDOUT, NULL, 1, meta_pid_to_tab);


	erts_fprintf(stderr, "ets: db_unref: meta_pid_to_fixed_tab common.memory_size = %ld\n",
		     erts_smp_atomic_read(&meta_pid_to_fixed_tab->common.memory_size));
	print_table(ERTS_PRINT_STDOUT, NULL, 1, meta_pid_to_fixed_tab);
	
#endif
#ifdef ERTS_SMP
	erts_smp_rwmtx_destroy(&tb->common.rwlock);
	erts_smp_mtx_destroy(&tb->common.fixlock);
#endif
	ASSERT(is_immed(tb->common.heir_data));
	erts_db_free(ERTS_ALC_T_DB_TABLE, tb, (void *) tb, sizeof(DbTable));		     
	ERTS_ETS_MISC_MEM_ADD(-sizeof(DbTable));
	return NULL;
    }
    return tb;
}

static ERTS_INLINE void db_init_lock(DbTable* tb, int use_frequent_read_lock,
				     char *rwname, char* fixname)
{
#ifdef ERTS_SMP
    erts_smp_rwmtx_opt_t rwmtx_opt = ERTS_SMP_THR_OPTS_DEFAULT_INITER;
    if (use_frequent_read_lock)
	rwmtx_opt.type = ERTS_SMP_RWMTX_TYPE_FREQUENT_READ;
#endif
    erts_refc_init(&tb->common.ref, 1);
    erts_refc_init(&tb->common.fixref, 0);
#ifdef ERTS_SMP
    erts_smp_rwmtx_init_opt_x(&tb->common.rwlock, &rwmtx_opt,
			      rwname, tb->common.the_name);
    erts_smp_mtx_init_x(&tb->common.fixlock, fixname, tb->common.the_name);
    tb->common.is_thread_safe = !(tb->common.status & DB_FINE_LOCKED);
#endif
}

static ERTS_INLINE void db_lock_take_over_ref(DbTable* tb, db_lock_kind_t kind)
{
#ifdef ERTS_SMP
    ASSERT(tb != meta_pid_to_tab && tb != meta_pid_to_fixed_tab);
    if (tb->common.type & DB_FINE_LOCKED) {
	if (kind == LCK_WRITE) {	   
	    erts_smp_rwmtx_rwlock(&tb->common.rwlock);
	    tb->common.is_thread_safe = 1;
	} else {	
	    erts_smp_rwmtx_rlock(&tb->common.rwlock);
	    ASSERT(!tb->common.is_thread_safe);
	}
    }
    else
    { 
	switch (kind) {
	case LCK_WRITE:
	case LCK_WRITE_REC:
	    erts_smp_rwmtx_rwlock(&tb->common.rwlock);
	    break;
	default:
	    erts_smp_rwmtx_rlock(&tb->common.rwlock);
	}
	ASSERT(tb->common.is_thread_safe);
    }
#endif
}

static ERTS_INLINE void db_lock(DbTable* tb, db_lock_kind_t kind)
{
    (void) db_ref(tb, kind);
#ifdef ERTS_SMP
    db_lock_take_over_ref(tb, kind);
#endif
}

static ERTS_INLINE void db_unlock(DbTable* tb, db_lock_kind_t kind)
{
#ifdef ERTS_SMP
    ASSERT(tb != meta_pid_to_tab && tb != meta_pid_to_fixed_tab);

    if (tb->common.type & DB_FINE_LOCKED) {
	if (tb->common.is_thread_safe) {
	    ASSERT(kind == LCK_WRITE);
	    tb->common.is_thread_safe = 0;
	    erts_smp_rwmtx_rwunlock(&tb->common.rwlock);
	}
	else {
	    ASSERT(kind != LCK_WRITE);
	    erts_smp_rwmtx_runlock(&tb->common.rwlock);
	}
    }
    else {
	ASSERT(tb->common.is_thread_safe);
	switch (kind) {
	case LCK_WRITE:
	case LCK_WRITE_REC:
	    erts_smp_rwmtx_rwunlock(&tb->common.rwlock);
	    break;
	default:
	    erts_smp_rwmtx_runlock(&tb->common.rwlock);
	}
    }
#endif
    (void) db_unref(tb, kind); /* May delete table... */
}


static ERTS_INLINE void db_meta_lock(DbTable* tb, db_lock_kind_t kind)
{
    ASSERT(tb == meta_pid_to_tab || tb == meta_pid_to_fixed_tab);
    ASSERT(kind != LCK_WRITE);
    /* As long as we only lock for READ we don't have to lock at all. */
}

static ERTS_INLINE void db_meta_unlock(DbTable* tb, db_lock_kind_t kind)
{
    ASSERT(tb == meta_pid_to_tab || tb == meta_pid_to_fixed_tab);
    ASSERT(kind != LCK_WRITE);
}

static ERTS_INLINE
DbTable* db_get_table_aux(Process *p,
			  Eterm id,
			  int what,
			  db_lock_kind_t kind,
			  int meta_already_locked)
{
    DbTable *tb = NULL;
    erts_smp_rwmtx_t *mtl = NULL;

    if (is_small(id)) {
	Uint slot = unsigned_val(id) & meta_main_tab_slot_mask;
	if (!meta_already_locked) {
	    mtl = get_meta_main_tab_lock(slot);
	    erts_smp_rwmtx_rlock(mtl);
	}
#if defined(ERTS_SMP) && defined(ERTS_ENABLE_LOCK_CHECK)
	else {
	    erts_smp_rwmtx_t *test_mtl = get_meta_main_tab_lock(slot);
	    ERTS_SMP_LC_ASSERT(erts_lc_rwmtx_is_rlocked(test_mtl)
			       || erts_lc_rwmtx_is_rwlocked(test_mtl));
	}
#endif
	if (slot < db_max_tabs && IS_SLOT_ALIVE(slot)) {
	    /* SMP: inc to prevent race, between unlock of meta_main_tab_lock
	     * and the table locking outside the meta_main_tab_lock
	     */
	    tb = db_ref(meta_main_tab[slot].u.tb, kind);
	}
    }
    else if (is_atom(id)) {
	struct meta_name_tab_entry* bucket = meta_name_tab_bucket(id,&mtl);
	if (!meta_already_locked)
	    erts_smp_rwmtx_rlock(mtl);
	else{
	    ERTS_SMP_LC_ASSERT(erts_lc_rwmtx_is_rlocked(mtl)
			       || erts_lc_rwmtx_is_rwlocked(mtl));
	    mtl = NULL;
	}

	if (bucket->pu.tb != NULL) {
	    if (is_atom(bucket->u.name_atom)) { /* single */
		if (bucket->u.name_atom == id) {
		    tb = db_ref(bucket->pu.tb, kind);
		}
	    }
	    else { /* multi */
		Uint cnt = unsigned_val(bucket->u.mcnt);
		Uint i;
		for (i=0; i<cnt; i++) {
		    if (bucket->pu.mvec[i].u.name_atom == id) {
			tb = db_ref(bucket->pu.mvec[i].pu.tb, kind);
			break;
		    }
		}
	    }
	}
    }
    if (tb) {
	db_lock_take_over_ref(tb, kind);
	if (tb->common.id != id
	    || ((tb->common.status & what) == 0 && p->id != tb->common.owner)) {
	    db_unlock(tb, kind);
	    tb = NULL;
	}
    }
    if (mtl)
	erts_smp_rwmtx_runlock(mtl);
    return tb;
}

static ERTS_INLINE
DbTable* db_get_table(Process *p,
		      Eterm id,
		      int what,
		      db_lock_kind_t kind)
{
    return db_get_table_aux(p, id, what, kind, 0);
}

/* Requires meta_main_tab_locks[slot] locked.
*/
static ERTS_INLINE void free_slot(int slot)
{
    ASSERT(!IS_SLOT_FREE(slot));
    erts_smp_spin_lock(&meta_main_tab_main_lock);
    SET_NEXT_FREE_SLOT(slot,meta_main_tab_first_free);
    meta_main_tab_first_free = slot;
    meta_main_tab_cnt--;
    erts_smp_spin_unlock(&meta_main_tab_main_lock);
}

static int insert_named_tab(Eterm name_atom, DbTable* tb, int have_lock)
{
    int ret = 0;
    erts_smp_rwmtx_t* rwlock;
    struct meta_name_tab_entry* new_entry;
    struct meta_name_tab_entry* bucket = meta_name_tab_bucket(name_atom,
							      &rwlock);
    if (!have_lock)
	erts_smp_rwmtx_rwlock(rwlock);

    if (bucket->pu.tb == NULL) { /* empty */
	new_entry = bucket;
    }
    else {
	struct meta_name_tab_entry* entries;
	Uint cnt;
	if (is_atom(bucket->u.name_atom)) { /* single */
	    size_t size;
	    if (bucket->u.name_atom == name_atom) {
		goto done;
	    }
	    cnt = 2;
	    size = sizeof(struct meta_name_tab_entry)*cnt;
	    entries = erts_db_alloc_nt(ERTS_ALC_T_DB_NTAB_ENT, size);
	    ERTS_ETS_MISC_MEM_ADD(size);
	    new_entry = &entries[0];
	    entries[1] = *bucket;
	}
	else { /* multi */
	    size_t size, old_size;
	    Uint i;
	    cnt = unsigned_val(bucket->u.mcnt);
	    for (i=0; i<cnt; i++) {
		if (bucket->pu.mvec[i].u.name_atom == name_atom) {
		    goto done;
		}
	    }
	    old_size = sizeof(struct meta_name_tab_entry)*cnt;
	    size = sizeof(struct meta_name_tab_entry)*(cnt+1);
	    entries = erts_db_realloc_nt(ERTS_ALC_T_DB_NTAB_ENT,
					 bucket->pu.mvec,
					 old_size,
					 size);
	    ERTS_ETS_MISC_MEM_ADD(size-old_size);
	    new_entry = &entries[cnt];
	    cnt++;
	}
	bucket->pu.mvec = entries;
	bucket->u.mcnt = make_small(cnt);
    }
    new_entry->pu.tb = tb;
    new_entry->u.name_atom = name_atom;
    ret = 1; /* Ok */

done:
    if (!have_lock)
	erts_smp_rwmtx_rwunlock(rwlock);
    return ret;
}

static int remove_named_tab(DbTable *tb, int have_lock)
{
    int ret = 0;
    erts_smp_rwmtx_t* rwlock;
    Eterm name_atom = tb->common.id;
    struct meta_name_tab_entry* bucket = meta_name_tab_bucket(name_atom,
							      &rwlock);
#ifdef ERTS_SMP
    if (!have_lock && erts_smp_rwmtx_tryrwlock(rwlock) == EBUSY) {
	/*
	 * We keep our increased refc over this op in order to
	 * prevent the table from disapearing.
	 */
	erts_smp_rwmtx_rwunlock(&tb->common.rwlock);
	erts_smp_rwmtx_rwlock(rwlock);
	erts_smp_rwmtx_rwlock(&tb->common.rwlock);
    }
#endif

    ERTS_SMP_LC_ASSERT(erts_lc_rwmtx_is_rwlocked(rwlock));

    if (bucket->pu.tb == NULL) {
	goto done;
    }
    else if (is_atom(bucket->u.name_atom)) { /* single */
	if (bucket->u.name_atom != name_atom) {
	    goto done;
	}
	bucket->pu.tb = NULL;
    }
    else { /* multi */
	Uint cnt = unsigned_val(bucket->u.mcnt);
	Uint i = 0;
	for (;;) {
	    if (bucket->pu.mvec[i].u.name_atom == name_atom) {
		break;
	    }
	    if (++i >= cnt) {
		goto done;
	    }
	}
	if (cnt == 2) { /* multi -> single */
	    size_t size;
	    struct meta_name_tab_entry* entries = bucket->pu.mvec;
	    *bucket = entries[1-i];
	    size = sizeof(struct meta_name_tab_entry)*cnt;
	    erts_db_free_nt(ERTS_ALC_T_DB_NTAB_ENT, entries, size);
	    ERTS_ETS_MISC_MEM_ADD(-size);
	    ASSERT(is_atom(bucket->u.name_atom));
	}
	else {
	    size_t size, old_size;
	    ASSERT(cnt > 2);
	    bucket->u.mcnt = make_small(--cnt);
	    if (i != cnt) {
		/* reposition last one before realloc destroys it */
		bucket->pu.mvec[i] = bucket->pu.mvec[cnt];
	    }
	    old_size = sizeof(struct meta_name_tab_entry)*(cnt+1);
	    size = sizeof(struct meta_name_tab_entry)*cnt;
	    bucket->pu.mvec = erts_db_realloc_nt(ERTS_ALC_T_DB_NTAB_ENT,
						 bucket->pu.mvec,
						 old_size,
						 size);
	    ERTS_ETS_MISC_MEM_ADD(size - old_size);
    
	}
    }
    ret = 1; /* Ok */

done:
    if (!have_lock)
	erts_smp_rwmtx_rwunlock(rwlock);
    return ret;
}

/* Do a fast fixation of a hash table.
** Must be matched by a local unfix before releasing table lock.
*/
static ERTS_INLINE void local_fix_table(DbTable* tb)
{
    erts_refc_inc(&tb->common.fixref, 1);
}	    
static ERTS_INLINE void local_unfix_table(DbTable* tb)
{	
    if (erts_refc_dectest(&tb->common.fixref, 0) == 0) {
	ASSERT(IS_HASH_TABLE(tb->common.status));
	db_unfix_table_hash(&(tb->hash));
    }
}


/*
 * BIFs.
 */

BIF_RETTYPE ets_safe_fixtable_2(BIF_ALIST_2)
{
    DbTable *tb;
    db_lock_kind_t kind;
#ifdef HARDDEBUG
    erts_fprintf(stderr,
		"ets:safe_fixtable(%T,%T); Process: %T, initial: %T:%T/%bpu\n",
		BIF_ARG_1, BIF_ARG_2, BIF_P->id,
		BIF_P->initial[0], BIF_P->initial[1], BIF_P->initial[2]);
#endif
    kind = (BIF_ARG_2 == am_true) ? LCK_READ : LCK_WRITE_REC; 

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, kind)) == NULL) {
	    BIF_ERROR(BIF_P, BADARG);
	}

    if (BIF_ARG_2 == am_true) {
	fix_table_locked(BIF_P, tb);
    }
    else if (BIF_ARG_2 == am_false) {
	if (IS_FIXED(tb)) {
	    unfix_table_locked(BIF_P, tb, &kind);
	}
    }
    else {
	db_unlock(tb, kind);
	BIF_ERROR(BIF_P, BADARG);
    }
    db_unlock(tb, kind);
    BIF_RET(am_true);
}


/* 
** Returns the first Key in a table 
*/
BIF_RETTYPE ets_first_1(BIF_ALIST_1)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ);

    if (!tb) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_first(BIF_P, tb, &ret);

    db_unlock(tb, LCK_READ);

    if (cret != DB_ERROR_NONE) {
	BIF_ERROR(BIF_P, BADARG);
    }
    BIF_RET(ret);
}

/* 
** The next BIF, given a key, return the "next" key 
*/
BIF_RETTYPE ets_next_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ);

    if (!tb) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_next(BIF_P, tb, BIF_ARG_2, &ret);

    db_unlock(tb, LCK_READ);

    if (cret != DB_ERROR_NONE) {
	BIF_ERROR(BIF_P, BADARG);
    }
    BIF_RET(ret);
}

/* 
** Returns the last Key in a table 
*/
BIF_RETTYPE ets_last_1(BIF_ALIST_1)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ);

    if (!tb) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_last(BIF_P, tb, &ret);

    db_unlock(tb, LCK_READ);

    if (cret != DB_ERROR_NONE) {
	BIF_ERROR(BIF_P, BADARG);
    }
    BIF_RET(ret);
}

/* 
** The prev BIF, given a key, return the "previous" key 
*/
BIF_RETTYPE ets_prev_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ);

    if (!tb) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_prev(BIF_P,tb,BIF_ARG_2,&ret);

    db_unlock(tb, LCK_READ);

    if (cret != DB_ERROR_NONE) {
	BIF_ERROR(BIF_P, BADARG);
    }
    BIF_RET(ret);
}

/* 
** update_element(Tab, Key, {Pos, Value})
** update_element(Tab, Key, [{Pos, Value}])
*/
BIF_RETTYPE ets_update_element_3(BIF_ALIST_3)
{
    DbTable* tb;
    int cret = DB_ERROR_BADITEM;
    Eterm list;
    Eterm iter;
    DeclareTmpHeap(cell,2,BIF_P);
    DbUpdateHandle handle;

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE_REC)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    UseTmpHeap(2,BIF_P);
    if (!(tb->common.status & (DB_SET | DB_ORDERED_SET))) {
	goto bail_out;
    }
    if (is_tuple(BIF_ARG_3)) {
	list = CONS(cell, BIF_ARG_3, NIL);
    }
    else {
	list = BIF_ARG_3;
    }

    if (!tb->common.meth->db_lookup_dbterm(tb, BIF_ARG_2, &handle)) {
	cret = DB_ERROR_BADKEY;
	goto bail_out;
    }

    /* First verify that list is ok to avoid nasty rollback scenarios
    */
    for (iter=list ; is_not_nil(iter); iter = CDR(list_val(iter))) {
	Eterm pv;
	Eterm* pvp;
	Sint position;

	if (is_not_list(iter)) {
	    goto finalize;
	}
	pv = CAR(list_val(iter));    /* {Pos,Value} */
	if (is_not_tuple(pv)) {
	    goto finalize;
	}
	pvp = tuple_val(pv);
	if (arityval(*pvp) != 2 || !is_small(pvp[1])) {
	    goto finalize;
	}
	position = signed_val(pvp[1]);
	if (position < 1 || position == tb->common.keypos || 
	    position > arityval(handle.dbterm->tpl[0])) {
	    goto finalize;
	}	
    }
    /* The point of no return, no failures from here on.
    */
    cret = DB_ERROR_NONE;

    for (iter=list ; is_not_nil(iter); iter = CDR(list_val(iter))) {
	Eterm* pvp = tuple_val(CAR(list_val(iter)));    /* {Pos,Value} */
	db_do_update_element(&handle, signed_val(pvp[1]), pvp[2]);
    }

finalize:
    tb->common.meth->db_finalize_dbterm(&handle);

bail_out:
    UnUseTmpHeap(2,BIF_P);
    db_unlock(tb, LCK_WRITE_REC);

    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(am_true);
    case DB_ERROR_BADKEY:
	BIF_RET(am_false);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
	break;
    }
}

/* 
** update_counter(Tab, Key, Incr) 
** update_counter(Tab, Key, {Upop}) 
** update_counter(Tab, Key, [{Upop}]) 
** Upop = {Pos,Incr} | {Pos,Incr,Threshold,WarpTo}
** Returns new value(s) (integer or [integer])
*/
BIF_RETTYPE ets_update_counter_3(BIF_ALIST_3)
{
    DbTable* tb;
    int cret = DB_ERROR_BADITEM;
    Eterm upop_list;
    int list_size;
    Eterm ret;  /* int or [int] */
    Eterm* ret_list_currp = NULL;
    Eterm* ret_list_prevp = NULL;
    Eterm iter;
    DeclareTmpHeap(cell,5,BIF_P);
    Eterm *tuple = cell+2;
    DbUpdateHandle handle;
    Uint halloc_size = 0; /* overestimated heap usage */
    Eterm* htop;          /* actual heap usage */
    Eterm* hstart;
    Eterm* hend;

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE_REC)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    UseTmpHeap(5,BIF_P);

    if (!(tb->common.status & (DB_SET | DB_ORDERED_SET))) {
	goto bail_out;
    }
    if (is_integer(BIF_ARG_3)) {  /* Incr */
	upop_list = CONS(cell, TUPLE2(tuple, make_small(tb->common.keypos+1),
				      BIF_ARG_3), NIL);
    }
    else if (is_tuple(BIF_ARG_3)) { /* {Upop} */
	upop_list = CONS(cell, BIF_ARG_3, NIL);
    }
    else { /* [{Upop}] (probably) */
	upop_list = BIF_ARG_3;
	ret_list_prevp = &ret;
    }

    if (!tb->common.meth->db_lookup_dbterm(tb, BIF_ARG_2, &handle)) {
	goto bail_out; /* key not found */
    }

    /* First verify that list is ok to avoid nasty rollback scenarios
    */
    list_size = 0;
    for (iter=upop_list ; is_not_nil(iter); iter = CDR(list_val(iter)),
	                                    list_size += 2) {
	Eterm upop;
	Eterm* tpl;
	Sint position;
	Eterm incr, warp, oldcnt;

	if (is_not_list(iter)) {
	    goto finalize;
	}
	upop = CAR(list_val(iter));
	if (is_not_tuple(upop)) {
	    goto finalize;
	}
	tpl = tuple_val(upop);
	switch (arityval(*tpl)) {
	case 4: /* threshold specified */
	    if (is_not_integer(tpl[3])) {
		goto finalize;
	    }
	    warp = tpl[4];
	    if (is_big(warp)) {
		halloc_size += BIG_NEED_SIZE(big_arity(warp));
	    }
	    else if (is_not_small(warp)) {
		goto finalize;
	    }
	    /* Fall through */
	case 2:
	    if (!is_small(tpl[1])) {
		goto finalize;
	    }
	    incr = tpl[2];
	    if (is_big(incr)) {
		halloc_size += BIG_NEED_SIZE(big_arity(incr));
	    }
	    else if (is_not_small(incr)) {
		goto finalize;
	    }
	    position = signed_val(tpl[1]);
	    if (position < 1 || position == tb->common.keypos ||
		position > arityval(handle.dbterm->tpl[0])) {
		goto finalize;
	    }
	    oldcnt = handle.dbterm->tpl[position];
	    if (is_big(oldcnt)) {
		halloc_size += BIG_NEED_SIZE(big_arity(oldcnt));
	    }
	    else if (is_not_small(oldcnt)) {
		goto finalize;
	    }
	    break;
	default:
	    goto finalize;
	}
	halloc_size += 2;  /* worst growth case: small(0)+small(0)=big(2) */
    }

    /* The point of no return, no failures from here on.
    */
    cret = DB_ERROR_NONE;

    if (ret_list_prevp) { /* Prepare to return a list */
	ret = NIL;
	halloc_size += list_size;
	hstart = HAlloc(BIF_P, halloc_size);
	ret_list_currp = hstart;
	htop = hstart + list_size;
	hend = hstart + halloc_size;
    }
    else {
	hstart = htop = HAlloc(BIF_P, halloc_size);
    }
    hend = hstart + halloc_size;

    for (iter=upop_list ; is_not_nil(iter); iter = CDR(list_val(iter))) {

	Eterm* tpl = tuple_val(CAR(list_val(iter)));
	Sint position = signed_val(tpl[1]);
	Eterm incr = tpl[2];
	Eterm oldcnt = handle.dbterm->tpl[position];
	Eterm newcnt = db_add_counter(&htop, oldcnt, incr);

	if (newcnt == NIL) {
	    cret = DB_ERROR_SYSRES; /* Can only happen if BIG_ARITY_MAX */
	    ret = NIL;              /* is reached, ie should not happen */
	    htop = hstart;
	    break;
	}
	ASSERT(is_integer(newcnt));

	if (arityval(*tpl) == 4) { /* Maybe warp it */
	    Eterm threshold = tpl[3];
	    if ((cmp(incr,make_small(0)) < 0) ? /* negative increment? */
		(cmp(newcnt,threshold) < 0) :  /* if negative, check if below */
		(cmp(newcnt,threshold) > 0)) { /* else check if above threshold */

		newcnt = tpl[4];
	    }
	}

	db_do_update_element(&handle,position,newcnt);

	if (ret_list_prevp) {
	    *ret_list_prevp = CONS(ret_list_currp,newcnt,NIL);
	    ret_list_prevp = &CDR(ret_list_currp);
	    ret_list_currp += 2;
	}
	else {
	    ret = newcnt;
	    break;	    
	}
    }

    ASSERT(is_integer(ret) || is_nil(ret) || 
	   (is_list(ret) && (list_val(ret)+list_size)==ret_list_currp));
    ASSERT(htop <= hend);

    HRelease(BIF_P,hend,htop);

finalize:
    tb->common.meth->db_finalize_dbterm(&handle);

bail_out:
    UnUseTmpHeap(5,BIF_P);
    db_unlock(tb, LCK_WRITE_REC);

    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
	break;
    }
}

/* 
** The put BIF 
*/
BIF_RETTYPE ets_insert_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret = DB_ERROR_NONE;
    Eterm lst;
    DbTableMethod* meth;
    db_lock_kind_t kind;

    CHECK_TABLES();

    /* Write lock table if more than one object to keep atomicy */
    kind = ((is_list(BIF_ARG_2) && CDR(list_val(BIF_ARG_2)) != NIL)
	    ? LCK_WRITE : LCK_WRITE_REC);

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, kind)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    if (BIF_ARG_2 == NIL) {
	db_unlock(tb, kind);
	BIF_RET(am_true);
    }
    meth = tb->common.meth;
    if (is_list(BIF_ARG_2)) {
	for (lst = BIF_ARG_2; is_list(lst); lst = CDR(list_val(lst))) {
	    if (is_not_tuple(CAR(list_val(lst))) || 
		(arityval(*tuple_val(CAR(list_val(lst)))) < tb->common.keypos)) {
		goto badarg;
	    }
	}
	if (lst != NIL) {
	    goto badarg;
	}
	for (lst = BIF_ARG_2; is_list(lst); lst = CDR(list_val(lst))) {
	    cret = meth->db_put(tb, CAR(list_val(lst)), 0);
	    if (cret != DB_ERROR_NONE)
		break;
	}
    } else {
	if (is_not_tuple(BIF_ARG_2) || 
	    (arityval(*tuple_val(BIF_ARG_2)) < tb->common.keypos)) {
	    goto badarg;
	}
	cret = meth->db_put(tb, BIF_ARG_2, 0);
    }

    db_unlock(tb, kind);
    
    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(am_true);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }
 badarg:
    db_unlock(tb, kind);
    BIF_ERROR(BIF_P, BADARG);    
}


/* 
** The put-if-not-already-there BIF... 
*/
BIF_RETTYPE ets_insert_new_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret = DB_ERROR_NONE;
    Eterm ret = am_true;
    Eterm obj;
    db_lock_kind_t kind;

    CHECK_TABLES();

    if (is_list(BIF_ARG_2)) {
	if (CDR(list_val(BIF_ARG_2)) != NIL) {
	    Eterm lst;
	    Eterm lookup_ret;
	    DbTableMethod* meth;

	    /* More than one object, use LCK_WRITE to keep atomicy */
	    kind = LCK_WRITE;
	    tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, kind);
	    if (tb == NULL) {
		BIF_ERROR(BIF_P, BADARG);
	    }
	    meth = tb->common.meth;
	    for (lst = BIF_ARG_2; is_list(lst); lst = CDR(list_val(lst))) {
		if (is_not_tuple(CAR(list_val(lst)))
		    || (arityval(*tuple_val(CAR(list_val(lst))))
			< tb->common.keypos)) {
		    goto badarg;
		}
	    }
	    if (lst != NIL) {
		goto badarg;
	    }    
	    for (lst = BIF_ARG_2; is_list(lst); lst = CDR(list_val(lst))) {
		cret = meth->db_member(tb, TERM_GETKEY(tb,CAR(list_val(lst))),
				       &lookup_ret);
		if ((cret != DB_ERROR_NONE) || (lookup_ret != am_false)) {
		    ret = am_false;
		    goto done;
		}
	    }
    
	    for (lst = BIF_ARG_2; is_list(lst); lst = CDR(list_val(lst))) {
		cret = meth->db_put(tb,CAR(list_val(lst)), 0);
		if (cret != DB_ERROR_NONE)
		    break;
	    }
	    goto done;
	}
	obj = CAR(list_val(BIF_ARG_2));
    }
    else {
	obj = BIF_ARG_2;
    }
    /* Only one object (or NIL) 
    */
    kind = LCK_WRITE_REC;
    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, kind)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    if (BIF_ARG_2 == NIL) {
	db_unlock(tb, kind);
	BIF_RET(am_true);
    }
    if (is_not_tuple(obj)
	|| (arityval(*tuple_val(obj)) < tb->common.keypos)) {
	goto badarg;
    }
    cret = tb->common.meth->db_put(tb, obj,
				   1); /* key_clash_fail */

done:
    db_unlock(tb, kind);
    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_BADKEY:
	BIF_RET(am_false);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }
 badarg:
    db_unlock(tb, kind);
    BIF_ERROR(BIF_P, BADARG);    
}

/*
** Rename a (possibly) named table
*/

BIF_RETTYPE ets_rename_2(BIF_ALIST_2)
{
    DbTable* tb;
    Eterm ret;
    erts_smp_rwmtx_t *lck1, *lck2;

#ifdef HARDDEBUG
    erts_fprintf(stderr,
		"ets:rename(%T,%T); Process: %T, initial: %T:%T/%bpu\n",
		BIF_ARG_1, BIF_ARG_2, BIF_P->id,
		BIF_P->initial[0], BIF_P->initial[1], BIF_P->initial[2]);
#endif


    if (is_not_atom(BIF_ARG_2)) {
	BIF_ERROR(BIF_P, BADARG);
    }

    (void) meta_name_tab_bucket(BIF_ARG_2, &lck1);

    if (is_small(BIF_ARG_1)) {
	Uint slot = unsigned_val(BIF_ARG_1) & meta_main_tab_slot_mask;
	lck2 = get_meta_main_tab_lock(slot);
    }
    else if (is_atom(BIF_ARG_1)) {
	(void) meta_name_tab_bucket(BIF_ARG_1, &lck2);
	if (lck1 == lck2)
	    lck2 = NULL;
	else if (lck1 > lck2) {
	    erts_smp_rwmtx_t *tmp = lck1;
	    lck1 = lck2;
	    lck2 = tmp;
	}
    }
    else {
	BIF_ERROR(BIF_P, BADARG);
    }

    erts_smp_rwmtx_rwlock(lck1);
    if (lck2)
	erts_smp_rwmtx_rwlock(lck2);

    tb = db_get_table_aux(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE, 1);
    if (!tb)
	goto badarg;

    if (is_not_atom(tb->common.id)) { /* Not a named table */
	tb->common.the_name = BIF_ARG_2;
	goto done;
    }

    if (!insert_named_tab(BIF_ARG_2, tb, 1))
	goto badarg;

    if (!remove_named_tab(tb, 1))
	erl_exit(1,"Could not find named tab %s", tb->common.id);

    tb->common.id = tb->common.the_name = BIF_ARG_2;

 done:
    ret = tb->common.id;
    db_unlock(tb, LCK_WRITE);
    erts_smp_rwmtx_rwunlock(lck1);
    if (lck2)
	erts_smp_rwmtx_rwunlock(lck2);
    BIF_RET(ret);
 badarg:
    if (tb)
	db_unlock(tb, LCK_WRITE);
    erts_smp_rwmtx_rwunlock(lck1);
    if (lck2)
	erts_smp_rwmtx_rwunlock(lck2);
    BIF_ERROR(BIF_P, BADARG);    
}


/* 
** The create table BIF     
** Args: (Name, Properties) 
*/

BIF_RETTYPE ets_new_2(BIF_ALIST_2)
{
    DbTable* tb = NULL;
    int slot;
    Eterm list;
    Eterm val;
    Eterm ret;
    Eterm heir;
    UWord heir_data;
    Uint32 status;
    Sint keypos;
    int is_named, is_fine_locked, frequent_read;
    int cret;
    DeclareTmpHeap(meta_tuple,3,BIF_P);
    DbTableMethod* meth;
    erts_smp_rwmtx_t *mmtl;

    if (is_not_atom(BIF_ARG_1)) {
	BIF_ERROR(BIF_P, BADARG);
    }
    if (is_not_nil(BIF_ARG_2) && is_not_list(BIF_ARG_2)) {
	BIF_ERROR(BIF_P, BADARG);
    }

    status = DB_NORMAL | DB_SET | DB_PROTECTED;
    keypos = 1;
    is_named = 0;
    is_fine_locked = 0;
    frequent_read = 0;
    heir = am_none;
    heir_data = (UWord) am_undefined;

    list = BIF_ARG_2;
    while(is_list(list)) {
	val = CAR(list_val(list));
	if (val == am_bag) {
	    status |= DB_BAG;
	    status &= ~(DB_SET | DB_DUPLICATE_BAG | DB_ORDERED_SET);
	}
	else if (val == am_duplicate_bag) {
	    status |= DB_DUPLICATE_BAG;
	    status &= ~(DB_SET | DB_BAG | DB_ORDERED_SET);
	}
	else if (val == am_ordered_set) {
	    status |= DB_ORDERED_SET;
	    status &= ~(DB_SET | DB_BAG | DB_DUPLICATE_BAG);
	}
	/*TT*/
	else if (is_tuple(val)) {
	    Eterm *tp = tuple_val(val);
	    if (arityval(tp[0]) == 2) {
		if (tp[1] == am_keypos
		    && is_small(tp[2]) && (signed_val(tp[2]) > 0)) {
		    keypos = signed_val(tp[2]);
		}		
		else if (tp[1] == am_write_concurrency) {
		    if (tp[2] == am_true) {
			is_fine_locked = 1;
		    } else if (tp[2] == am_false) {
			is_fine_locked = 0;
		    } else break;
		}
		else if (tp[1] == am_read_concurrency) {
		    if (tp[2] == am_true) {
			frequent_read = 1;
		    } else if (tp[2] == am_false) {
			frequent_read = 0;
		    } else break;
		}
		else if (tp[1] == am_heir && tp[2] == am_none) {
		    heir = am_none;
		    heir_data = am_undefined;
		}
		else break;
	    }
	    else if (arityval(tp[0]) == 3 && tp[1] == am_heir
		     && is_internal_pid(tp[2])) {
		heir = tp[2];
		heir_data = tp[3];
	    }
	    else break;
	}
	else if (val == am_public) {
	    status |= DB_PUBLIC;
	    status &= ~(DB_PROTECTED|DB_PRIVATE);
	}
	else if (val == am_private) {
	    status |= DB_PRIVATE;
	    status &= ~(DB_PROTECTED|DB_PUBLIC);
	}
	else if (val == am_named_table) {
	    is_named = 1;
	}
	else if (val == am_set || val == am_protected)
	    ;
	else break;

	list = CDR(list_val(list));
    }
    if (is_not_nil(list)) { /* bad opt or not a well formed list */
	BIF_ERROR(BIF_P, BADARG);
    }
    if (IS_HASH_TABLE(status)) {
	meth = &db_hash;
	#ifdef ERTS_SMP
	if (is_fine_locked && !(status & DB_PRIVATE)) {
	    status |= DB_FINE_LOCKED;
	}
	#endif
    }
    else if (IS_TREE_TABLE(status)) {
	meth = &db_tree;
    }
    else {
	BIF_ERROR(BIF_P, BADARG);
    }

#ifdef ERTS_SMP
    if (frequent_read && !(status & DB_PRIVATE))
	status |= DB_FREQ_READ;
#endif

    /* we create table outside any table lock
     * and take the unusal cost of destroy table if it
     * fails to find a slot 
     */
    {
        DbTable init_tb;

	erts_smp_atomic_init(&init_tb.common.memory_size, 0);
	tb = (DbTable*) erts_db_alloc(ERTS_ALC_T_DB_TABLE,
				      &init_tb, sizeof(DbTable));
	ERTS_ETS_MISC_MEM_ADD(sizeof(DbTable));
	erts_smp_atomic_init(&tb->common.memory_size,
			     erts_smp_atomic_read(&init_tb.common.memory_size));
    }

    tb->common.meth = meth;
    tb->common.the_name = BIF_ARG_1;
    tb->common.status = status;    
#ifdef ERTS_SMP
    tb->common.type = status & ERTS_ETS_TABLE_TYPES;
    /* Note, 'type' is *read only* from now on... */
#endif
    db_init_lock(tb, status & (DB_FINE_LOCKED|DB_FREQ_READ),
		 "db_tab", "db_tab_fix");
    tb->common.keypos = keypos;
    tb->common.owner = BIF_P->id;
    set_heir(BIF_P, tb, heir, heir_data);

    erts_smp_atomic_init(&tb->common.nitems, 0);

    tb->common.fixations = NULL;

    cret = meth->db_create(BIF_P, tb);
    ASSERT(cret == DB_ERROR_NONE);

    erts_smp_spin_lock(&meta_main_tab_main_lock);

    if (meta_main_tab_cnt >= db_max_tabs) {
	erts_smp_spin_unlock(&meta_main_tab_main_lock);
	erts_send_error_to_logger_str(BIF_P->group_leader,
				      "** Too many db tables **\n");
	free_heir_data(tb);
	tb->common.meth->db_free_table(tb);
	erts_db_free(ERTS_ALC_T_DB_TABLE, tb, (void *) tb, sizeof(DbTable));
	ERTS_ETS_MISC_MEM_ADD(-sizeof(DbTable));
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    }

    slot = meta_main_tab_first_free;
    ASSERT(slot>=0 && slot<db_max_tabs);
    meta_main_tab_first_free = GET_NEXT_FREE_SLOT(slot);
    meta_main_tab_cnt++;

    if (is_named) {
	ret = BIF_ARG_1;
    }
    else {
	ret = make_small(slot | meta_main_tab_seq_cnt);
	meta_main_tab_seq_cnt += meta_main_tab_seq_incr;
	ASSERT((unsigned_val(ret) & meta_main_tab_slot_mask) == slot);
    }
    erts_smp_spin_unlock(&meta_main_tab_main_lock);

    tb->common.id = ret;
    tb->common.slot = slot;           /* store slot for erase */

    mmtl = get_meta_main_tab_lock(slot);
    erts_smp_rwmtx_rwlock(mmtl);
    meta_main_tab[slot].u.tb = tb;
    ASSERT(IS_SLOT_ALIVE(slot));
    erts_smp_rwmtx_rwunlock(mmtl);

    if (is_named && !insert_named_tab(BIF_ARG_1, tb, 0)) {
	mmtl = get_meta_main_tab_lock(slot);
	erts_smp_rwmtx_rwlock(mmtl);
	free_slot(slot);
	erts_smp_rwmtx_rwunlock(mmtl);

	db_lock_take_over_ref(tb,LCK_WRITE);
	free_heir_data(tb);
	tb->common.meth->db_free_table(tb);
	db_unlock(tb,LCK_WRITE);
	BIF_ERROR(BIF_P, BADARG);
    }
    
    BIF_P->flags |= F_USING_DB; /* So we can remove tb if p dies */

#ifdef HARDDEBUG
    erts_fprintf(stderr,
		"ets:new(%T,%T)=%T; Process: %T, initial: %T:%T/%bpu\n",
		 BIF_ARG_1, BIF_ARG_2, ret, BIF_P->id,
		 BIF_P->initial[0], BIF_P->initial[1], BIF_P->initial[2]);
	erts_fprintf(stderr, "ets: new: meta_pid_to_tab common.memory_size = %ld\n",
		     erts_smp_atomic_read(&meta_pid_to_tab->common.memory_size));
	erts_fprintf(stderr, "ets: new: meta_pid_to_fixed_tab common.memory_size = %ld\n",
		     erts_smp_atomic_read(&meta_pid_to_fixed_tab->common.memory_size));
#endif

    UseTmpHeap(3,BIF_P);

    db_meta_lock(meta_pid_to_tab, LCK_WRITE_REC);
    if (db_put_hash(meta_pid_to_tab,
		    TUPLE2(meta_tuple, BIF_P->id, make_small(slot)),
		    0) != DB_ERROR_NONE) {
	erl_exit(1,"Could not update ets metadata.");
    }
    db_meta_unlock(meta_pid_to_tab, LCK_WRITE_REC);

    UnUseTmpHeap(3,BIF_P);

    BIF_RET(ret);
}

/* 
** The lookup BIF 
*/
BIF_RETTYPE ets_lookup_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_get(BIF_P, tb, BIF_ARG_2, &ret);

    db_unlock(tb, LCK_READ);

    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }

}

/* 
** The lookup BIF 
*/
BIF_RETTYPE ets_member_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_member(tb, BIF_ARG_2, &ret);

    db_unlock(tb, LCK_READ);

    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }

}

/* 
** Get an element from a term
** get_element_3(Tab, Key, Index)
** return the element or a list of elements if bag
*/
BIF_RETTYPE ets_lookup_element_3(BIF_ALIST_3)
{
    DbTable* tb;
    Sint index;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    if (is_not_small(BIF_ARG_3) || ((index = signed_val(BIF_ARG_3)) < 1)) {
	db_unlock(tb, LCK_READ);
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_get_element(BIF_P, tb, 
					   BIF_ARG_2, index, &ret);
    db_unlock(tb, LCK_READ);
    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }
}

/* 
 * BIF to erase a whole table and release all memory it holds 
 */
BIF_RETTYPE ets_delete_1(BIF_ALIST_1)
{
    int trap;
    DbTable* tb;
    erts_smp_rwmtx_t *mmtl;

#ifdef HARDDEBUG
    erts_fprintf(stderr,
		"ets:delete(%T); Process: %T, initial: %T:%T/%bpu\n",
		BIF_ARG_1, BIF_P->id,
		BIF_P->initial[0], BIF_P->initial[1], BIF_P->initial[2]);
#endif

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    
    /*
     * Clear all access bits to prevent any ets operation to access the
     * table while it is being deleted.
     */
    tb->common.status &= ~(DB_PROTECTED|DB_PUBLIC|DB_PRIVATE);
    tb->common.status |= DB_DELETE;

    mmtl = get_meta_main_tab_lock(tb->common.slot);
#ifdef ERTS_SMP
    if (erts_smp_rwmtx_tryrwlock(mmtl) == EBUSY) {
	/*
	 * We keep our increased refc over this op in order to
	 * prevent the table from disapearing.
	 */
	erts_smp_rwmtx_rwunlock(&tb->common.rwlock);
	erts_smp_rwmtx_rwlock(mmtl);
	erts_smp_rwmtx_rwlock(&tb->common.rwlock);
    }
#endif
    /* We must keep the slot, to be found by db_proc_dead() if process dies */
    MARK_SLOT_DEAD(tb->common.slot);
    erts_smp_rwmtx_rwunlock(mmtl);
    if (is_atom(tb->common.id))
	remove_named_tab(tb, 0);
    
    if (tb->common.owner != BIF_P->id) {
	DeclareTmpHeap(meta_tuple,3,BIF_P);

	/*
	 * The table is being deleted by a process other than its owner.
	 * To make sure that the table will be completely deleted if the
	 * current process will be killed (e.g. by an EXIT signal), we will
	 * now transfer the ownership to the current process.
	 */
	UseTmpHeap(3,BIF_P);
	db_meta_lock(meta_pid_to_tab, LCK_WRITE_REC);
	db_erase_bag_exact2(meta_pid_to_tab, tb->common.owner,
			    make_small(tb->common.slot));

	BIF_P->flags |= F_USING_DB;
	tb->common.owner = BIF_P->id;

	db_put_hash(meta_pid_to_tab,
		    TUPLE2(meta_tuple,BIF_P->id,make_small(tb->common.slot)),
		    0);
	db_meta_unlock(meta_pid_to_tab, LCK_WRITE_REC);
	UnUseTmpHeap(3,BIF_P);
    }    
    /* disable inheritance */
    free_heir_data(tb);
    tb->common.heir = am_none;

    free_fixations_locked(tb);

    trap = free_table_cont(BIF_P, tb, 1, 1);
    db_unlock(tb, LCK_WRITE);
    if (trap) {
	/*
	 * Package the DbTable* pointer into a bignum so that it can be safely
	 * passed through a trap. We used to pass the DbTable* pointer directly
	 * (it looks like an continuation pointer), but that is will crash the
	 * emulator if this BIF is call traced.
	 */
#if HALFWORD_HEAP
	Eterm *hp = HAlloc(BIF_P, 3);
	hp[0] = make_pos_bignum_header(2);
	*((UWord *) (UWord) (hp+1)) = (UWord) tb;
#else
	Eterm *hp = HAlloc(BIF_P, 2);
	hp[0] = make_pos_bignum_header(1);
	hp[1] = (Eterm) tb;
#endif
	BIF_TRAP1(&ets_delete_continue_exp, BIF_P, make_big(hp));
    }
    else {
	BIF_RET(am_true);
    }
}

/* 
** BIF ets:give_away(Tab, Pid, GiftData)
*/
BIF_RETTYPE ets_give_away_3(BIF_ALIST_3)
{
    Process* to_proc = NULL;
    ErtsProcLocks to_locks = ERTS_PROC_LOCK_MAIN;
    DeclareTmpHeap(buf,5,BIF_P);
    Eterm to_pid = BIF_ARG_2;
    Eterm from_pid;
    DbTable* tb = NULL;

    if (!is_internal_pid(to_pid)) {
	goto badarg;
    }
    to_proc = erts_pid2proc(BIF_P, ERTS_PROC_LOCK_MAIN, to_pid, to_locks);
    if (to_proc == NULL) {
	goto badarg;
    }

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE)) == NULL
	|| tb->common.owner != BIF_P->id) {
	goto badarg;
    }
    from_pid = tb->common.owner;
    if (to_pid == from_pid) {
	goto badarg;  /* or should we be idempotent? return false maybe */
    }

    UseTmpHeap(5,BIF_P);
    db_meta_lock(meta_pid_to_tab, LCK_WRITE_REC);
    db_erase_bag_exact2(meta_pid_to_tab, tb->common.owner,
			make_small(tb->common.slot));

    to_proc->flags |= F_USING_DB;
    tb->common.owner = to_pid;

    db_put_hash(meta_pid_to_tab,
		TUPLE2(buf,to_pid,make_small(tb->common.slot)),
		0);
    db_meta_unlock(meta_pid_to_tab, LCK_WRITE_REC);

    db_unlock(tb,LCK_WRITE);
    erts_send_message(BIF_P, to_proc, &to_locks,
		      TUPLE4(buf, am_ETS_TRANSFER, tb->common.id, from_pid, BIF_ARG_3), 
		      0);
    erts_smp_proc_unlock(to_proc, to_locks);
    UnUseTmpHeap(5,BIF_P);
    BIF_RET(am_true);

badarg:
    if (to_proc != NULL && to_proc != BIF_P) erts_smp_proc_unlock(to_proc, to_locks);
    if (tb != NULL) db_unlock(tb, LCK_WRITE);
    BIF_ERROR(BIF_P, BADARG);
}

BIF_RETTYPE ets_setopts_2(BIF_ALIST_2)
{
    DbTable* tb = NULL;
    Eterm* tp;
    Eterm opt;
    Eterm heir = THE_NON_VALUE;
    UWord heir_data = (UWord) THE_NON_VALUE;
    Uint32 protection = 0;
    DeclareTmpHeap(fakelist,2,BIF_P);
    Eterm tail;

    UseTmpHeap(2,BIF_P);
    for (tail = is_tuple(BIF_ARG_2) ? CONS(fakelist, BIF_ARG_2, NIL) : BIF_ARG_2;	
	  is_list(tail);
	  tail = CDR(list_val(tail))) {

	opt = CAR(list_val(tail));
	if (!is_tuple(opt) || (tp = tuple_val(opt), arityval(tp[0]) < 2)) { 
	    goto badarg;
	}

	switch (tp[1]) {
	case am_heir:
	    if (heir != THE_NON_VALUE) goto badarg;
	    heir = tp[2];
	    if (arityval(tp[0]) == 2 && heir == am_none) {
		heir_data = am_undefined;
	    } 
	    else if (arityval(tp[0]) == 3 && is_internal_pid(heir)) {
		heir_data = tp[3];
	    }
	    else goto badarg;
	    break;

	case am_protection:
	    if (arityval(tp[0]) != 2 || protection != 0) goto badarg; 
	    switch (tp[2]) {
	    case am_private: protection = DB_PRIVATE; break;
	    case am_protected: protection = DB_PROTECTED; break;
	    case am_public: protection = DB_PUBLIC; break;
	    default: goto badarg;
	    }
	    break;

	default: goto badarg;
	}
    }

    if (tail != NIL
	|| (tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE)) == NULL
	|| tb->common.owner != BIF_P->id) {
	goto badarg;
    }

    if (heir_data != THE_NON_VALUE) {
	free_heir_data(tb);
	set_heir(BIF_P, tb, heir, heir_data);
    }
    if (protection) {
	tb->common.status &= ~(DB_PRIVATE|DB_PROTECTED|DB_PUBLIC);
	tb->common.status |= protection;
    }

    db_unlock (tb,LCK_WRITE);
    UnUseTmpHeap(2,BIF_P);
    BIF_RET(am_true);

badarg:
    UnUseTmpHeap(2,BIF_P);
    if (tb != NULL) {
	db_unlock(tb,LCK_WRITE);
    }
    BIF_ERROR(BIF_P, BADARG);
}

/* 
** BIF to erase a whole table and release all memory it holds 
*/
BIF_RETTYPE ets_delete_all_objects_1(BIF_ALIST_1)
{
    DbTable* tb;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    tb->common.meth->db_delete_all_objects(BIF_P, tb);

    db_unlock(tb, LCK_WRITE);

    BIF_RET(am_true);
}

/* 
** Erase an object with given key, or maybe several objects if we have a bag  
** Called as db_erase(Tab, Key), where Key is element 1 of the
** object(s) we want to erase                                  
*/
BIF_RETTYPE ets_delete_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE_REC)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_erase(tb,BIF_ARG_2,&ret);

    db_unlock(tb, LCK_WRITE_REC);

    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }
}

/* 
** Erase a specific object, or maybe several objects if we have a bag  
*/
BIF_RETTYPE ets_delete_object_2(BIF_ALIST_2)
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE_REC)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    if (is_not_tuple(BIF_ARG_2) || 
	(arityval(*tuple_val(BIF_ARG_2)) < tb->common.keypos)) {
	db_unlock(tb, LCK_WRITE_REC);
	BIF_ERROR(BIF_P, BADARG);
    }

    cret = tb->common.meth->db_erase_object(tb, BIF_ARG_2, &ret);
    db_unlock(tb, LCK_WRITE_REC);

    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }
}

/*
** This is for trapping, cannot be called directly.
*/
static BIF_RETTYPE ets_select_delete_1(Process *p, Eterm a1)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    Eterm ret;
    Eterm *tptr;
    db_lock_kind_t kind = LCK_WRITE_REC;
    
    CHECK_TABLES();
    ASSERT(is_tuple(a1));
    tptr = tuple_val(a1);
    ASSERT(arityval(*tptr) >= 1);
    
    if ((tb = db_get_table(p, tptr[1], DB_WRITE, kind)) == NULL) {
	BIF_ERROR(p,BADARG);
    }

    cret = tb->common.meth->db_select_delete_continue(p,tb,a1,&ret);

    if(!DID_TRAP(p,ret) && ITERATION_SAFETY(p,tb) != ITER_SAFE) {  
	unfix_table_locked(p, tb, &kind);
    }

    db_unlock(tb, kind);

    switch (cret) {
    case DB_ERROR_NONE:      
	ERTS_BIF_PREP_RET(result, ret);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, p, BADARG);
	break;
    }
    erts_match_set_release_result(p);

    return result;
}
    

BIF_RETTYPE ets_select_delete_2(BIF_ALIST_2)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    Eterm ret;
    enum DbIterSafety safety;

    CHECK_TABLES();

    if(eq(BIF_ARG_2, ms_delete_all)) {
	int nitems;
	if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE)) == NULL) {
	    BIF_ERROR(BIF_P, BADARG);
	}
	nitems = erts_smp_atomic_read(&tb->common.nitems);
	tb->common.meth->db_delete_all_objects(BIF_P, tb);
	db_unlock(tb, LCK_WRITE);
	BIF_RET(erts_make_integer(nitems,BIF_P));
    }

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_WRITE, LCK_WRITE_REC)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }
    cret = tb->common.meth->db_select_delete(BIF_P, tb, BIF_ARG_2, &ret);

    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P,tb);
    }
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_WRITE_REC);

    switch (cret) {
    case DB_ERROR_NONE:	
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }

    erts_match_set_release_result(BIF_P);

    return result;
}

/* 
** Return a list of tables on this node 
*/
BIF_RETTYPE ets_all_0(BIF_ALIST_0)
{
    DbTable* tb;
    Eterm previous;
    int i, j;
    Eterm* hp;
    Eterm* hendp;
    int t_tabs_cnt;
    int t_max_tabs;

    erts_smp_spin_lock(&meta_main_tab_main_lock);
    t_tabs_cnt = meta_main_tab_cnt;
    t_max_tabs = db_max_tabs;
    erts_smp_spin_unlock(&meta_main_tab_main_lock);

    hp = HAlloc(BIF_P, 2*t_tabs_cnt);
    hendp = hp + 2*t_tabs_cnt;

    previous = NIL;
    j = 0;
    for(i = 0; (i < t_max_tabs && j < t_tabs_cnt); i++) {
	erts_smp_rwmtx_t *mmtl = get_meta_main_tab_lock(i);
	erts_smp_rwmtx_rlock(mmtl);
	if (IS_SLOT_ALIVE(i)) {
	    j++;
	    tb = meta_main_tab[i].u.tb;
	    previous = CONS(hp, tb->common.id, previous);
	    hp += 2;
	}
	erts_smp_rwmtx_runlock(mmtl);
    }
    HRelease(BIF_P, hendp, hp);
    BIF_RET(previous);
}


/*
** db_slot(Db, Slot) -> [Items].
*/
BIF_RETTYPE ets_slot_2(BIF_ALIST_2) 
{
    DbTable* tb;
    int cret;
    Eterm ret;

    CHECK_TABLES();

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    /* The slot number is checked in table specific code. */
    cret = tb->common.meth->db_slot(BIF_P, tb, BIF_ARG_2, &ret);
    db_unlock(tb, LCK_READ);
    switch (cret) {
    case DB_ERROR_NONE:
	BIF_RET(ret);
    case DB_ERROR_SYSRES:
	BIF_ERROR(BIF_P, SYSTEM_LIMIT);
    default:
	BIF_ERROR(BIF_P, BADARG);
    }
}

/* 
** The match BIF,  called as ets:match(Table, Pattern), ets:match(Continuation) or ets:match(Table,Pattern,ChunkSize).
*/

BIF_RETTYPE ets_match_1(BIF_ALIST_1)
{
    return ets_select_1(BIF_P, BIF_ARG_1);
}

BIF_RETTYPE ets_match_2(BIF_ALIST_2)
{
    Eterm ms;
    DeclareTmpHeap(buff,8,BIF_P);
    Eterm *hp = buff;
    Eterm res;

    UseTmpHeap(8,BIF_P);
    ms = CONS(hp, am_DollarDollar, NIL);
    hp += 2;
    ms = TUPLE3(hp, BIF_ARG_2, NIL, ms); 
    hp += 4;
    ms = CONS(hp, ms, NIL);
    res = ets_select_2(BIF_P, BIF_ARG_1, ms);
    UnUseTmpHeap(8,BIF_P);
    return res;
}

BIF_RETTYPE ets_match_3(BIF_ALIST_3)
{
    Eterm ms;
    DeclareTmpHeap(buff,8,BIF_P);
    Eterm *hp = buff;
    Eterm res;

    UseTmpHeap(8,BIF_P);
    ms = CONS(hp, am_DollarDollar, NIL);
    hp += 2;
    ms = TUPLE3(hp, BIF_ARG_2, NIL, ms); 
    hp += 4;
    ms = CONS(hp, ms, NIL);
    res = ets_select_3(BIF_P, BIF_ARG_1, ms, BIF_ARG_3);
    UnUseTmpHeap(8,BIF_P);
    return res;
}


BIF_RETTYPE ets_select_3(BIF_ALIST_3)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    Eterm ret;
    Sint chunk_size;
    enum DbIterSafety safety;

    CHECK_TABLES();

    /* Chunk size strictly greater than 0 */
    if (is_not_small(BIF_ARG_3) || (chunk_size = signed_val(BIF_ARG_3)) <= 0) {
	BIF_ERROR(BIF_P, BADARG);
    }
    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }
    cret = tb->common.meth->db_select_chunk(BIF_P, tb,
					    BIF_ARG_2, chunk_size, 
					    0 /* not reversed */,
					    &ret);
    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P, tb);
    }
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_READ);

    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }

    erts_match_set_release_result(BIF_P);

    return result;
}


/* We get here instead of in the real BIF when trapping */
static BIF_RETTYPE ets_select_trap_1(Process *p, Eterm a1)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    Eterm ret;
    Eterm *tptr;
    db_lock_kind_t kind = LCK_READ;

    CHECK_TABLES();

    tptr = tuple_val(a1);
    ASSERT(arityval(*tptr) >= 1)

    if ((tb = db_get_table(p, tptr[1], DB_READ, kind)) == NULL) {
	BIF_ERROR(p, BADARG);
    }

    cret = tb->common.meth->db_select_continue(p, tb, a1,
					       &ret);

    if (!DID_TRAP(p,ret) && ITERATION_SAFETY(p,tb) != ITER_SAFE) {
	unfix_table_locked(p, tb, &kind);
    }
    db_unlock(tb, kind);

    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, p, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, p, BADARG);
	break;
    }

    erts_match_set_release_result(p);

    return result;
}


BIF_RETTYPE ets_select_1(BIF_ALIST_1)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    Eterm ret;
    Eterm *tptr;
    enum DbIterSafety safety;

    CHECK_TABLES();

    /*
     * Make sure that the table exists.
     */

    if (!is_tuple(BIF_ARG_1)) {
	if (BIF_ARG_1 == am_EOT) {
	    BIF_RET(am_EOT);
	}
	BIF_ERROR(BIF_P, BADARG);
    }
    tptr = tuple_val(BIF_ARG_1);
    if (arityval(*tptr) < 1 ||
	(tb = db_get_table(BIF_P, tptr[1], DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }

    cret = tb->common.meth->db_select_continue(BIF_P,tb,
					       BIF_ARG_1, &ret);

    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P, tb);
    }
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_READ);

    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }

    erts_match_set_release_result(BIF_P);

    return result;
}

BIF_RETTYPE ets_select_2(BIF_ALIST_2)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    enum DbIterSafety safety;
    Eterm ret;

    CHECK_TABLES();

    /*
     * Make sure that the table exists.
     */

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }

    cret = tb->common.meth->db_select(BIF_P, tb, BIF_ARG_2,
				      0, &ret);

    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P, tb);
    }    
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_READ);

    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }

    erts_match_set_release_result(BIF_P);

    return result;
}

/* We get here instead of in the real BIF when trapping */
static BIF_RETTYPE ets_select_count_1(Process *p, Eterm a1)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    Eterm ret;
    Eterm *tptr;
    db_lock_kind_t kind = LCK_READ;

    CHECK_TABLES();

    tptr = tuple_val(a1);
    ASSERT(arityval(*tptr) >= 1)
    if ((tb = db_get_table(p, tptr[1], DB_READ, kind)) == NULL) {
	BIF_ERROR(p, BADARG);
    }

    cret = tb->common.meth->db_select_count_continue(p, tb, a1, &ret);

    if (!DID_TRAP(p,ret) && ITERATION_SAFETY(p,tb) != ITER_SAFE) {
	unfix_table_locked(p, tb, &kind);
    }
    db_unlock(tb, kind);

    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, p, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, p, BADARG);
	break;
    }

    erts_match_set_release_result(p);

    return result;
}

BIF_RETTYPE ets_select_count_2(BIF_ALIST_2)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    enum DbIterSafety safety;
    Eterm ret;

    CHECK_TABLES();
    /*
     * Make sure that the table exists.
     */

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }
    cret = tb->common.meth->db_select_count(BIF_P,tb,BIF_ARG_2, &ret);

    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P, tb);
    }
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_READ);
    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }

    erts_match_set_release_result(BIF_P);

    return result;
}


BIF_RETTYPE ets_select_reverse_3(BIF_ALIST_3)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    enum DbIterSafety safety;
    Eterm ret;
    Sint chunk_size;

    CHECK_TABLES();
    /*
     * Make sure that the table exists.
     */

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    
    /* Chunk size strictly greater than 0 */
    if (is_not_small(BIF_ARG_3) || (chunk_size = signed_val(BIF_ARG_3)) <= 0) {
	db_unlock(tb, LCK_READ);
	BIF_ERROR(BIF_P, BADARG);
    }
    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }
    cret = tb->common.meth->db_select_chunk(BIF_P,tb,
					    BIF_ARG_2, chunk_size, 
					    1 /* reversed */, &ret);
    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P, tb);
    }
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_READ);
    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }
    erts_match_set_release_result(BIF_P);
    return result;
}

BIF_RETTYPE ets_select_reverse_1(BIF_ALIST_1)
{
    return ets_select_1(BIF_P, BIF_ARG_1);
}

BIF_RETTYPE ets_select_reverse_2(BIF_ALIST_2)
{
    BIF_RETTYPE result;
    DbTable* tb;
    int cret;
    enum DbIterSafety safety;
    Eterm ret;

    CHECK_TABLES();
    /*
     * Make sure that the table exists.
     */

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_READ, LCK_READ)) == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }
    safety = ITERATION_SAFETY(BIF_P,tb);
    if (safety == ITER_UNSAFE) {
	local_fix_table(tb);
    }
    cret = tb->common.meth->db_select(BIF_P,tb,BIF_ARG_2,
				      1 /*reversed*/, &ret);

    if (DID_TRAP(BIF_P,ret) && safety != ITER_SAFE) {
	fix_table_locked(BIF_P, tb);
    }    
    if (safety == ITER_UNSAFE) {
	local_unfix_table(tb);
    }
    db_unlock(tb, LCK_READ);
    switch (cret) {
    case DB_ERROR_NONE:
	ERTS_BIF_PREP_RET(result, ret);
	break;
    case DB_ERROR_SYSRES:
	ERTS_BIF_PREP_ERROR(result, BIF_P, SYSTEM_LIMIT);
	break;
    default:
	ERTS_BIF_PREP_ERROR(result, BIF_P, BADARG);
	break;
    }
    erts_match_set_release_result(BIF_P);
    return result;
}


/* 
** ets:match_object(Continuation), ets:match_object(Table, Pattern), ets:match_object(Table,Pattern,ChunkSize) 
*/
BIF_RETTYPE ets_match_object_1(BIF_ALIST_1)
{
    return ets_select_1(BIF_P, BIF_ARG_1);
}

BIF_RETTYPE ets_match_object_2(BIF_ALIST_2)
{
    Eterm ms;
    DeclareTmpHeap(buff,8,BIF_P);
    Eterm *hp = buff;
    Eterm res;

    UseTmpHeap(8,BIF_P);
    ms = CONS(hp, am_DollarUnderscore, NIL);
    hp += 2;
    ms = TUPLE3(hp, BIF_ARG_2, NIL, ms); 
    hp += 4;
    ms = CONS(hp, ms, NIL);
    res = ets_select_2(BIF_P, BIF_ARG_1, ms);
    UnUseTmpHeap(8,BIF_P);
    return res;
}

BIF_RETTYPE ets_match_object_3(BIF_ALIST_3)
{
    Eterm ms;
    DeclareTmpHeap(buff,8,BIF_P);
    Eterm *hp = buff;
    Eterm res;

    UseTmpHeap(8,BIF_P);
    ms = CONS(hp, am_DollarUnderscore, NIL);
    hp += 2;
    ms = TUPLE3(hp, BIF_ARG_2, NIL, ms); 
    hp += 4;
    ms = CONS(hp, ms, NIL);
    res = ets_select_3(BIF_P, BIF_ARG_1, ms, BIF_ARG_3);
    UnUseTmpHeap(8,BIF_P);
    return res;
}

/* 
 * BIF to extract information about a particular table.
 */ 

BIF_RETTYPE ets_info_1(BIF_ALIST_1)
{
    static Eterm fields[] = {am_protection, am_keypos, am_type, am_named_table,
	am_node, am_size, am_name, am_heir, am_owner, am_memory};
    Eterm results[sizeof(fields)/sizeof(Eterm)];
    DbTable* tb;
    Eterm res;
    int i;
    Eterm* hp;
    /*Process* rp = NULL;*/
    Eterm owner;

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_INFO, LCK_READ)) == NULL) {
	if (is_atom(BIF_ARG_1) || is_small(BIF_ARG_1)) {
	    BIF_RET(am_undefined);
	}
	BIF_ERROR(BIF_P, BADARG);
    }

    owner = tb->common.owner;

    /* If/when we implement lockless private tables:
    if ((tb->common.status & DB_PRIVATE) && owner != BIF_P->id) {
	db_unlock(tb, LCK_READ);
	rp = erts_pid2proc_not_running(BIF_P, ERTS_PROC_LOCK_MAIN,
				       owner, ERTS_PROC_LOCK_MAIN);
	if (rp == NULL) {
	    BIF_RET(am_undefined);
	}
	if (rp == ERTS_PROC_LOCK_BUSY) {
	    ERTS_BIF_YIELD1(bif_export[BIF_ets_info_1], BIF_P, BIF_ARG_1);
	}
	if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_INFO, LCK_READ)) == NULL
	    || tb->common.owner != owner) {
	    if (BIF_P != rp)
		erts_smp_proc_unlock(rp, ERTS_PROC_LOCK_MAIN);
	    if (is_atom(BIF_ARG_1) || is_small(BIF_ARG_1)) {
		BIF_RET(am_undefined);
	    }
	    BIF_ERROR(BIF_P, BADARG);
	}
    }*/
    for (i = 0; i < sizeof(fields)/sizeof(Eterm); i++) {
	results[i] = table_info(BIF_P, tb, fields[i]);
	ASSERT(is_value(results[i]));
    }
    db_unlock(tb, LCK_READ);

    /*if (rp != NULL && rp != BIF_P)
	erts_smp_proc_unlock(rp, ERTS_PROC_LOCK_MAIN);*/

    hp = HAlloc(BIF_P, 5*sizeof(fields)/sizeof(Eterm));
    res = NIL;
    for (i = 0; i < sizeof(fields)/sizeof(Eterm); i++) {
	Eterm tuple;
	tuple = TUPLE2(hp, fields[i], results[i]);
	hp += 3;
	res = CONS(hp, tuple, res);
	hp += 2;
    }
    BIF_RET(res);
}

/* 
 * BIF to extract information about a particular table.
 */ 

BIF_RETTYPE ets_info_2(BIF_ALIST_2)
{
    DbTable* tb;
    Eterm ret = THE_NON_VALUE;

    if ((tb = db_get_table(BIF_P, BIF_ARG_1, DB_INFO, LCK_READ)) == NULL) {
	if (is_atom(BIF_ARG_1) || is_small(BIF_ARG_1)) {
	    BIF_RET(am_undefined);
	}
	BIF_ERROR(BIF_P, BADARG);
    }
    ret = table_info(BIF_P, tb, BIF_ARG_2);
    db_unlock(tb, LCK_READ);
    if (is_non_value(ret)) {
	BIF_ERROR(BIF_P, BADARG);
    }
    BIF_RET(ret);
}


BIF_RETTYPE ets_is_compiled_ms_1(BIF_ALIST_1)
{
    if (erts_db_is_compiled_ms(BIF_ARG_1)) {
	BIF_RET(am_true);
    } else {
	BIF_RET(am_false);
    }
}

BIF_RETTYPE ets_match_spec_compile_1(BIF_ALIST_1)
{
    Binary *mp = db_match_set_compile(BIF_P, BIF_ARG_1, DCOMP_TABLE);
    Eterm *hp;
    if (mp == NULL) {
	BIF_ERROR(BIF_P, BADARG);
    }

    hp = HAlloc(BIF_P, PROC_BIN_SIZE);

    BIF_RET(erts_mk_magic_binary_term(&hp, &MSO(BIF_P), mp));
}

BIF_RETTYPE ets_match_spec_run_r_3(BIF_ALIST_3)
{
    Eterm ret = BIF_ARG_3;
    int i = 0;
    Eterm *hp;
    Eterm lst;
    ProcBin *bp;
    Binary *mp;
    Eterm res;
    Uint32 dummy;
    Uint sz;

    if (!(is_list(BIF_ARG_1) || BIF_ARG_1 == NIL) || !is_binary(BIF_ARG_2)) {
    error:
	BIF_ERROR(BIF_P, BADARG);
    }
    
    bp = (ProcBin*) binary_val(BIF_ARG_2);
    if (thing_subtag(bp->thing_word) != REFC_BINARY_SUBTAG) {
	goto error;
    }
    mp = bp->val;
    if (!IsMatchProgBinary(mp)) {
	goto error;
    }

    if (BIF_ARG_1 == NIL) {
	BIF_RET(BIF_ARG_3);
    }
    for (lst = BIF_ARG_1; is_list(lst); lst = CDR(list_val(lst))) {
	if (++i > CONTEXT_REDS) {
	    BUMP_ALL_REDS(BIF_P);
	    BIF_TRAP3(bif_export[BIF_ets_match_spec_run_r_3],
		      BIF_P,lst,BIF_ARG_2,ret);
	}
	res = db_prog_match(BIF_P, mp, CAR(list_val(lst)), NULL, 0, &dummy);
	if (is_value(res)) {
	    sz = size_object(res);
	    hp = HAlloc(BIF_P, sz + 2);
	    res = copy_struct(res, sz, &hp, &MSO(BIF_P));
	    ret = CONS(hp,res,ret);
	    /*hp += 2;*/
	} 
    }
    if (lst != NIL) {
	goto error;
    }
    BIF_RET2(ret,i);
}


/*
** External interface (NOT BIF's)
*/


/* Init the db */

void init_db(void)
{
    DbTable init_tb;
    int i;
    extern BeamInstr* em_apply_bif;
    Eterm *hp;
    unsigned bits;
    size_t size;

#ifdef ERTS_SMP
    erts_smp_rwmtx_opt_t rwmtx_opt = ERTS_SMP_THR_OPTS_DEFAULT_INITER;
    rwmtx_opt.type = ERTS_SMP_RWMTX_TYPE_FREQUENT_READ;
    rwmtx_opt.lived = ERTS_SMP_RWMTX_LONG_LIVED;

    meta_main_tab_locks = erts_alloc(ERTS_ALC_T_DB_TABLES,
				     (sizeof(erts_meta_main_tab_lock_t)
				      * (ERTS_META_MAIN_TAB_LOCK_TAB_SIZE+1)));

    if ((((Uint) meta_main_tab_locks) & ERTS_CACHE_LINE_MASK) != 0)
	meta_main_tab_locks = ((erts_meta_main_tab_lock_t *)
			       ((((Uint) meta_main_tab_locks)
				 & ~ERTS_CACHE_LINE_MASK)
				+ ERTS_CACHE_LINE_SIZE));

    ASSERT((((Uint) meta_main_tab_locks) & ERTS_CACHE_LINE_MASK) == 0);

    for (i = 0; i < ERTS_META_MAIN_TAB_LOCK_TAB_SIZE; i++) {
	erts_smp_rwmtx_init_opt_x(&meta_main_tab_locks[i].rwmtx, &rwmtx_opt,
				  "meta_main_tab_slot", make_small(i));
    }
    erts_smp_spinlock_init(&meta_main_tab_main_lock, "meta_main_tab_main");
    for (i=0; i<META_NAME_TAB_LOCK_CNT; i++) {
	erts_smp_rwmtx_init_opt_x(&meta_name_tab_rwlocks[i].lck, &rwmtx_opt,
				  "meta_name_tab", make_small(i));
    }
#endif

    erts_smp_atomic_init(&erts_ets_misc_mem_size, 0);
    db_initialize_util();

    if (user_requested_db_max_tabs < DB_DEF_MAX_TABS)
	db_max_tabs = DB_DEF_MAX_TABS;
    else
	db_max_tabs = user_requested_db_max_tabs;

    bits = erts_fit_in_bits(db_max_tabs-1);
    if (bits > SMALL_BITS) {
	erl_exit(1,"Max limit for ets tabled too high %u (max %u).",
		 db_max_tabs, 1L<<SMALL_BITS);
    }
    meta_main_tab_slot_mask = (1L<<bits) - 1;
    meta_main_tab_seq_incr = (1L<<bits);

    size = sizeof(*meta_main_tab)*db_max_tabs;
    meta_main_tab = erts_db_alloc_nt(ERTS_ALC_T_DB_TABLES, size);
    ERTS_ETS_MISC_MEM_ADD(size);

    meta_main_tab_cnt = 0;
    for (i=1; i<db_max_tabs; i++) {
	SET_NEXT_FREE_SLOT(i-1,i);
    }
    SET_NEXT_FREE_SLOT(db_max_tabs-1, (Uint)-1);
    meta_main_tab_first_free = 0;

    meta_name_tab_mask = (1L<<(bits-1)) - 1; /* At least half the size of main tab */
    size = sizeof(struct meta_name_tab_entry)*(meta_name_tab_mask+1);
    meta_name_tab = erts_db_alloc_nt(ERTS_ALC_T_DB_TABLES, size);
    ERTS_ETS_MISC_MEM_ADD(size);

    for (i=0; i<=meta_name_tab_mask; i++) {
	meta_name_tab[i].pu.tb = NULL;
	meta_name_tab[i].u.name_atom = NIL;
    }

    db_initialize_hash();
    db_initialize_tree();

    /*TT*/
    /* Create meta table invertion. */
    erts_smp_atomic_init(&init_tb.common.memory_size, 0);
    meta_pid_to_tab = (DbTable*) erts_db_alloc(ERTS_ALC_T_DB_TABLE,
					       &init_tb,
					       sizeof(DbTable));
    ERTS_ETS_MISC_MEM_ADD(sizeof(DbTable));
    erts_smp_atomic_init(&meta_pid_to_tab->common.memory_size,
			 erts_smp_atomic_read(&init_tb.common.memory_size));

    meta_pid_to_tab->common.id = NIL;
    meta_pid_to_tab->common.the_name = am_true;
    meta_pid_to_tab->common.status = (DB_NORMAL | DB_BAG | DB_PUBLIC | DB_FINE_LOCKED);
#ifdef ERTS_SMP
    meta_pid_to_tab->common.type
	= meta_pid_to_tab->common.status & ERTS_ETS_TABLE_TYPES;
    /* Note, 'type' is *read only* from now on... */
    meta_pid_to_tab->common.is_thread_safe = 0;
#endif
    meta_pid_to_tab->common.keypos = 1;
    meta_pid_to_tab->common.owner  = NIL;
    erts_smp_atomic_init(&meta_pid_to_tab->common.nitems, 0);
    meta_pid_to_tab->common.slot   = -1;
    meta_pid_to_tab->common.meth   = &db_hash;

    erts_refc_init(&meta_pid_to_tab->common.ref, 1);
    erts_refc_init(&meta_pid_to_tab->common.fixref, 0);
    /* Neither rwlock or fixlock used
    db_init_lock(meta_pid_to_tab, "meta_pid_to_tab", "meta_pid_to_tab_FIX");*/

    if (db_create_hash(NULL, meta_pid_to_tab) != DB_ERROR_NONE) {
	erl_exit(1,"Unable to create ets metadata tables.");
    }

    erts_smp_atomic_set(&init_tb.common.memory_size, 0);
    meta_pid_to_fixed_tab = (DbTable*) erts_db_alloc(ERTS_ALC_T_DB_TABLE,
						     &init_tb,
						     sizeof(DbTable));
    ERTS_ETS_MISC_MEM_ADD(sizeof(DbTable));
    erts_smp_atomic_init(&meta_pid_to_fixed_tab->common.memory_size,
			 erts_smp_atomic_read(&init_tb.common.memory_size));

    meta_pid_to_fixed_tab->common.id = NIL;
    meta_pid_to_fixed_tab->common.the_name = am_true;
    meta_pid_to_fixed_tab->common.status = (DB_NORMAL | DB_BAG | DB_PUBLIC | DB_FINE_LOCKED);
#ifdef ERTS_SMP
    meta_pid_to_fixed_tab->common.type
	= meta_pid_to_fixed_tab->common.status & ERTS_ETS_TABLE_TYPES;
    /* Note, 'type' is *read only* from now on... */
    meta_pid_to_fixed_tab->common.is_thread_safe = 0;
#endif
    meta_pid_to_fixed_tab->common.keypos = 1;
    meta_pid_to_fixed_tab->common.owner  = NIL;
    erts_smp_atomic_init(&meta_pid_to_fixed_tab->common.nitems, 0);
    meta_pid_to_fixed_tab->common.slot   = -1;
    meta_pid_to_fixed_tab->common.meth   = &db_hash;

    erts_refc_init(&meta_pid_to_fixed_tab->common.ref, 1);
    erts_refc_init(&meta_pid_to_fixed_tab->common.fixref, 0);
    /* Neither rwlock or fixlock used
    db_init_lock(meta_pid_to_fixed_tab, "meta_pid_to_fixed_tab", "meta_pid_to_fixed_tab_FIX");*/

    if (db_create_hash(NULL, meta_pid_to_fixed_tab) != DB_ERROR_NONE) {
	erl_exit(1,"Unable to create ets metadata tables.");
    }

    /* Non visual BIF to trap to. */
    memset(&ets_select_delete_continue_exp, 0, sizeof(Export));
    ets_select_delete_continue_exp.address = 
	&ets_select_delete_continue_exp.code[3];
    ets_select_delete_continue_exp.code[0] = am_ets;
    ets_select_delete_continue_exp.code[1] = am_atom_put("delete_trap",11);
    ets_select_delete_continue_exp.code[2] = 1;
    ets_select_delete_continue_exp.code[3] =
	(BeamInstr) em_apply_bif;
    ets_select_delete_continue_exp.code[4] = 
	(BeamInstr) &ets_select_delete_1;

    /* Non visual BIF to trap to. */
    memset(&ets_select_count_continue_exp, 0, sizeof(Export));
    ets_select_count_continue_exp.address = 
	&ets_select_count_continue_exp.code[3];
    ets_select_count_continue_exp.code[0] = am_ets;
    ets_select_count_continue_exp.code[1] = am_atom_put("count_trap",11);
    ets_select_count_continue_exp.code[2] = 1;
    ets_select_count_continue_exp.code[3] =
	(BeamInstr) em_apply_bif;
    ets_select_count_continue_exp.code[4] = 
	(BeamInstr) &ets_select_count_1;

    /* Non visual BIF to trap to. */
    memset(&ets_select_continue_exp, 0, sizeof(Export));
    ets_select_continue_exp.address = 
	&ets_select_continue_exp.code[3];
    ets_select_continue_exp.code[0] = am_ets;
    ets_select_continue_exp.code[1] = am_atom_put("select_trap",11);
    ets_select_continue_exp.code[2] = 1;
    ets_select_continue_exp.code[3] =
	(BeamInstr) em_apply_bif;
    ets_select_continue_exp.code[4] = 
	(BeamInstr) &ets_select_trap_1;

    /* Non visual BIF to trap to. */
    memset(&ets_delete_continue_exp, 0, sizeof(Export));
    ets_delete_continue_exp.address = &ets_delete_continue_exp.code[3];
    ets_delete_continue_exp.code[0] = am_ets;
    ets_delete_continue_exp.code[1] = am_atom_put("delete_trap",11);
    ets_delete_continue_exp.code[2] = 1;
    ets_delete_continue_exp.code[3] = (BeamInstr) em_apply_bif;
    ets_delete_continue_exp.code[4] = (BeamInstr) &ets_delete_trap;

    hp = ms_delete_all_buff;
    ms_delete_all = CONS(hp, am_true, NIL);
    hp += 2;
    ms_delete_all = TUPLE3(hp,am_Underscore,NIL,ms_delete_all);
    hp +=4;
    ms_delete_all = CONS(hp, ms_delete_all,NIL);
}

#define ARRAY_CHUNK 100

typedef enum {
    ErtsDbProcCleanupProgressTables,
    ErtsDbProcCleanupProgressFixations,
    ErtsDbProcCleanupProgressDone,
} ErtsDbProcCleanupProgress;

typedef enum {
    ErtsDbProcCleanupOpGetTables,
    ErtsDbProcCleanupOpDeleteTables,
    ErtsDbProcCleanupOpGetFixations,
    ErtsDbProcCleanupOpDeleteFixations,
    ErtsDbProcCleanupOpDone
} ErtsDbProcCleanupOperation;

typedef struct {
    ErtsDbProcCleanupProgress progress;
    ErtsDbProcCleanupOperation op;
    struct {
	Eterm arr[ARRAY_CHUNK];
	int size;
	int ix;
	int clean_ix;
    } slots;
} ErtsDbProcCleanupState;


static void
proc_exit_cleanup_tables_meta_data(Eterm pid, ErtsDbProcCleanupState *state)
{
    ASSERT(state->slots.clean_ix <= state->slots.ix);
    if (state->slots.clean_ix < state->slots.ix) {
	db_meta_lock(meta_pid_to_tab, LCK_WRITE_REC);
	if (state->slots.size < ARRAY_CHUNK
	    && state->slots.ix == state->slots.size) {
	    Eterm dummy;
	    db_erase_hash(meta_pid_to_tab,pid,&dummy);
	}
	else {
	    int ix;
	    /* Need to erase each explicitly */
	    for (ix = state->slots.clean_ix; ix < state->slots.ix; ix++)
		db_erase_bag_exact2(meta_pid_to_tab,
				    pid,
				    state->slots.arr[ix]);
	}
	db_meta_unlock(meta_pid_to_tab, LCK_WRITE_REC);
	state->slots.clean_ix = state->slots.ix;
    }
}

static void
proc_exit_cleanup_fixations_meta_data(Eterm pid, ErtsDbProcCleanupState *state)
{
    ASSERT(state->slots.clean_ix <= state->slots.ix);
    if (state->slots.clean_ix < state->slots.ix) {
	db_meta_lock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
	if (state->slots.size < ARRAY_CHUNK
	    && state->slots.ix == state->slots.size) {
	    Eterm dummy;
	    db_erase_hash(meta_pid_to_fixed_tab,pid,&dummy);
	}
	else {
	    int ix;
	    /* Need to erase each explicitly */
	    for (ix = state->slots.clean_ix; ix < state->slots.ix; ix++)
		db_erase_bag_exact2(meta_pid_to_fixed_tab,
				    pid,
				    state->slots.arr[ix]);
	}
	db_meta_unlock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
	state->slots.clean_ix = state->slots.ix;
    }
}

/* In: Table LCK_WRITE
** Return TRUE : ok, table not mine and NOT locked anymore.
** Return FALSE: failed, table still mine (LCK_WRITE)
*/
static int give_away_to_heir(Process* p, DbTable* tb)
{
    Process* to_proc;
    ErtsProcLocks to_locks = ERTS_PROC_LOCK_MAIN;
    DeclareTmpHeap(buf,5,p);
    Eterm to_pid;
    UWord heir_data;

    ASSERT(tb->common.owner == p->id);
    ASSERT(is_internal_pid(tb->common.heir));
    ASSERT(tb->common.heir != p->id);
retry:
    to_pid = tb->common.heir;
    to_proc = erts_pid2proc_opt(p, ERTS_PROC_LOCK_MAIN,
				to_pid, to_locks,
				ERTS_P2P_FLG_TRY_LOCK);
    if (to_proc == ERTS_PROC_LOCK_BUSY) {
	db_ref(tb, LCK_NONE); /* while unlocked */
	db_unlock(tb,LCK_WRITE);    
	to_proc = erts_pid2proc(p, ERTS_PROC_LOCK_MAIN,
				to_pid, to_locks);    
	db_lock(tb,LCK_WRITE);
	tb = db_unref(tb, LCK_NONE);
	ASSERT(tb != NULL);
    
	if (tb->common.owner != p->id) {
	    if (to_proc != NULL ) {
		erts_smp_proc_unlock(to_proc, to_locks);
	    }
	    db_unlock(tb,LCK_WRITE);
	    return !0; /* ok, someone already gave my table away */
	}
	if (tb->common.heir != to_pid) {  /* someone changed the heir */ 
	    if (to_proc != NULL ) {
		erts_smp_proc_unlock(to_proc, to_locks);
	    }
	    if (to_pid == p->id || to_pid == am_none) {
		return 0; /* no real heir, table still mine */
	    }
	    goto retry;
	}
    }
    if (to_proc == NULL) {
	return 0; /* heir not alive, table still mine */
    }
    if (erts_cmp_timeval(&to_proc->started, &tb->common.heir_started) != 0) {
	erts_smp_proc_unlock(to_proc, to_locks);
	return 0; /* heir dead and pid reused, table still mine */
    }
    UseTmpHeap(5,p);
    db_meta_lock(meta_pid_to_tab, LCK_WRITE_REC);
    db_erase_bag_exact2(meta_pid_to_tab, tb->common.owner,
			make_small(tb->common.slot));
    
    to_proc->flags |= F_USING_DB;
    tb->common.owner = to_pid;
    
    db_put_hash(meta_pid_to_tab,
		TUPLE2(buf,to_pid,make_small(tb->common.slot)),
		0);
    db_meta_unlock(meta_pid_to_tab, LCK_WRITE_REC);
    UnUseTmpHeap(5,p);
    db_unlock(tb,LCK_WRITE);
    heir_data = tb->common.heir_data;
    if (!is_immed(heir_data)) {
	Eterm* tpv = DBTERM_BUF((DbTerm*)heir_data); /* tuple_val */
	ASSERT(arityval(*tpv) == 1);
	heir_data = tpv[1];
    }
    erts_send_message(p, to_proc, &to_locks,
		      TUPLE4(buf, am_ETS_TRANSFER, tb->common.id, p->id, heir_data), 
		      0);
    erts_smp_proc_unlock(to_proc, to_locks);
    return !0;
}

/*
 * erts_db_process_exiting() is called when a process terminates.
 * It returns 0 when completely done, and !0 when it wants to
 * yield. c_p->u.exit_data can hold a pointer to a state while
 * yielding.
 */
#define ERTS_DB_INTERNAL_ERROR(LSTR) \
  erl_exit(ERTS_ABORT_EXIT, "%s:%d:erts_db_process_exiting(): " LSTR "\n", \
	   __FILE__, __LINE__)

int
erts_db_process_exiting(Process *c_p, ErtsProcLocks c_p_locks)
{
    ErtsDbProcCleanupState *state = (ErtsDbProcCleanupState *) c_p->u.exit_data;
    Eterm pid = c_p->id;
    ErtsDbProcCleanupState default_state;
    int ret;

    if (!state) {
	state = &default_state;
	state->progress = ErtsDbProcCleanupProgressTables;
	state->op = ErtsDbProcCleanupOpGetTables;
    }

    while (!0) {
	switch (state->op) {
	case ErtsDbProcCleanupOpGetTables:
	    state->slots.size = ARRAY_CHUNK;
	    db_meta_lock(meta_pid_to_tab, LCK_READ);
	    ret = db_get_element_array(meta_pid_to_tab,
				       pid,
				       2,
				       state->slots.arr,
				       &state->slots.size);
	    db_meta_unlock(meta_pid_to_tab, LCK_READ);
	    if (ret == DB_ERROR_BADKEY) {
		/* Done with tables; now fixations */
		state->progress = ErtsDbProcCleanupProgressFixations;
		state->op = ErtsDbProcCleanupOpGetFixations;
		break;
	    } else if (ret != DB_ERROR_NONE) {
		ERTS_DB_INTERNAL_ERROR("Inconsistent ets table metadata");
	    }

	    state->slots.ix = 0;
	    state->slots.clean_ix = 0;
	    state->op = ErtsDbProcCleanupOpDeleteTables;
	    /* Fall through */

	case ErtsDbProcCleanupOpDeleteTables:

	    while (state->slots.ix < state->slots.size) {
		DbTable *tb = NULL;
		Sint ix = unsigned_val(state->slots.arr[state->slots.ix]);
		erts_smp_rwmtx_t *mmtl = get_meta_main_tab_lock(ix);
		erts_smp_rwmtx_rlock(mmtl);
		if (!IS_SLOT_FREE(ix)) {
		    tb = db_ref(GET_ANY_SLOT_TAB(ix), LCK_WRITE);
		    ASSERT(tb);
		}
		erts_smp_rwmtx_runlock(mmtl);
		if (tb) {
		    int do_yield;
		    db_lock_take_over_ref(tb, LCK_WRITE);
		    /* Ownership may have changed since
		       we looked up the table. */
		    if (tb->common.owner != pid) {
			do_yield = 0;
			db_unlock(tb, LCK_WRITE);
		    }
		    else if (tb->common.heir != am_none
			     && tb->common.heir != pid
			     && give_away_to_heir(c_p, tb)) {
			do_yield = 0;
		    }
		    else {
			int first_call;
#ifdef HARDDEBUG
			erts_fprintf(stderr,
				     "erts_db_process_exiting(); Table: %T, "
				     "Process: %T\n",
				     tb->common.id, pid);
#endif
			first_call = (tb->common.status & DB_DELETE) == 0;
			if (first_call) {
			    /* Clear all access bits. */
			    tb->common.status &= ~(DB_PROTECTED
						   | DB_PUBLIC
						   | DB_PRIVATE);
			    tb->common.status |= DB_DELETE;

			    if (is_atom(tb->common.id))
				remove_named_tab(tb, 0);

			    free_heir_data(tb);
			    free_fixations_locked(tb);
			}

			do_yield = free_table_cont(c_p, tb, first_call, 0);
			db_unlock(tb, LCK_WRITE);
		    }		    
		    if (do_yield)
			goto yield;
		}
		state->slots.ix++;
		if (ERTS_BIF_REDS_LEFT(c_p) <= 0)
		    goto yield;
	    }

	    proc_exit_cleanup_tables_meta_data(pid, state);
	    state->op = ErtsDbProcCleanupOpGetTables;
	    break;

	case ErtsDbProcCleanupOpGetFixations:
	    state->slots.size = ARRAY_CHUNK;
	    db_meta_lock(meta_pid_to_fixed_tab, LCK_READ);
	    ret = db_get_element_array(meta_pid_to_fixed_tab, 
				       pid,
				       2,
				       state->slots.arr,
				       &state->slots.size);
	    db_meta_unlock(meta_pid_to_fixed_tab, LCK_READ);

	    if (ret == DB_ERROR_BADKEY) {
		/* Done */
		state->progress = ErtsDbProcCleanupProgressDone;
		state->op = ErtsDbProcCleanupOpDone;
		break;
	    } else if (ret != DB_ERROR_NONE) {
		ERTS_DB_INTERNAL_ERROR("Inconsistent ets fix table metadata");
	    }

	    state->slots.ix = 0;
	    state->slots.clean_ix = 0;
	    state->op = ErtsDbProcCleanupOpDeleteFixations;
	    /* Fall through */

	case ErtsDbProcCleanupOpDeleteFixations:

	    while (state->slots.ix < state->slots.size) {
		DbTable *tb = NULL;
		Sint ix = unsigned_val(state->slots.arr[state->slots.ix]);
		erts_smp_rwmtx_t *mmtl = get_meta_main_tab_lock(ix);
		erts_smp_rwmtx_rlock(mmtl);
		if (IS_SLOT_ALIVE(ix)) {
		    tb = db_ref(meta_main_tab[ix].u.tb, LCK_WRITE_REC);
		    ASSERT(tb);
		}
		erts_smp_rwmtx_runlock(mmtl);
		if (tb) {
		    int reds;
		    DbFixation** pp;

		    db_lock_take_over_ref(tb, LCK_WRITE_REC);
		    #ifdef ERTS_SMP
		    erts_smp_mtx_lock(&tb->common.fixlock);
		    #endif
		    reds = 10;
		    
		    for (pp = &tb->common.fixations; *pp != NULL;
			  pp = &(*pp)->next) {
			if ((*pp)->pid == pid) {
			    DbFixation* fix = *pp;
			    erts_refc_add(&tb->common.fixref,-fix->counter,0);
			    *pp = fix->next;
			    erts_db_free(ERTS_ALC_T_DB_FIXATION,
					 tb, fix, sizeof(DbFixation));
			    ERTS_ETS_MISC_MEM_ADD(-sizeof(DbFixation));
			    break;
			}
		    }
		    #ifdef ERTS_SMP
		    erts_smp_mtx_unlock(&tb->common.fixlock);
		    #endif
		    if (!IS_FIXED(tb) && IS_HASH_TABLE(tb->common.status)) {
			db_unfix_table_hash(&(tb->hash));
			reds += 40;
		    }
		    db_unlock(tb, LCK_WRITE_REC);
		    BUMP_REDS(c_p, reds);
		}
		state->slots.ix++;
		if (ERTS_BIF_REDS_LEFT(c_p) <= 0)
		    goto yield;
	    }

	    proc_exit_cleanup_fixations_meta_data(pid, state);
	    state->op = ErtsDbProcCleanupOpGetFixations;
	    break;

	case ErtsDbProcCleanupOpDone:

	    if (state != &default_state)
		erts_free(ERTS_ALC_T_DB_PROC_CLEANUP, state);
	    c_p->u.exit_data = NULL;
	    return 0;

	default:
	    ERTS_DB_INTERNAL_ERROR("Bad internal state");
	}
    }

 yield:

    switch (state->progress) {
    case ErtsDbProcCleanupProgressTables:
	proc_exit_cleanup_tables_meta_data(pid, state);
	break;
    case ErtsDbProcCleanupProgressFixations:
	proc_exit_cleanup_fixations_meta_data(pid, state);
	break;
    default:
	break;
    }

    ASSERT(c_p->u.exit_data == (void *) state
	   || state == &default_state);

    if (state == &default_state) {
	c_p->u.exit_data = erts_alloc(ERTS_ALC_T_DB_PROC_CLEANUP,
				      sizeof(ErtsDbProcCleanupState));
	sys_memcpy(c_p->u.exit_data,
		   (void*) state,
		   sizeof(ErtsDbProcCleanupState));
    }

    return !0;
}

/*  SMP note: table only need to be LCK_READ locked */
static void fix_table_locked(Process* p, DbTable* tb)
{
    DbFixation *fix;
    DeclareTmpHeap(meta_tuple,3,p);

#ifdef ERTS_SMP
    erts_smp_mtx_lock(&tb->common.fixlock);
#endif
    erts_refc_inc(&tb->common.fixref,1);
    fix = tb->common.fixations;
    if (fix == NULL) { 
	get_now(&(tb->common.megasec),
		&(tb->common.sec), 
		&(tb->common.microsec));
    }
    else {
	for (; fix != NULL; fix = fix->next) {
	    if (fix->pid == p->id) {
		++(fix->counter);
#ifdef ERTS_SMP
		erts_smp_mtx_unlock(&tb->common.fixlock);
#endif
		return;
	    }
	}
    }
    fix = (DbFixation *) erts_db_alloc(ERTS_ALC_T_DB_FIXATION,
				       tb, sizeof(DbFixation));
    ERTS_ETS_MISC_MEM_ADD(sizeof(DbFixation));
    fix->pid = p->id;
    fix->counter = 1;
    fix->next = tb->common.fixations;
    tb->common.fixations = fix;
#ifdef ERTS_SMP
    erts_smp_mtx_unlock(&tb->common.fixlock);
#endif
    p->flags |= F_USING_DB;        
    UseTmpHeap(3,p);
    db_meta_lock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
    if (db_put_hash(meta_pid_to_fixed_tab,
		    TUPLE2(meta_tuple, p->id, make_small(tb->common.slot)),
		    0) != DB_ERROR_NONE) {
	UnUseTmpHeap(3,p);
	erl_exit(1,"Could not insert ets metadata in safe_fixtable.");
    }	
    UnUseTmpHeap(3,p);
    db_meta_unlock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
}

/* SMP note: May re-lock table 
*/
static void unfix_table_locked(Process* p,  DbTable* tb,
			       db_lock_kind_t* kind_p)
{
    DbFixation** pp;

#ifdef ERTS_SMP
    erts_smp_mtx_lock(&tb->common.fixlock);
#endif
    for (pp = &tb->common.fixations; *pp != NULL; pp = &(*pp)->next) {
	if ((*pp)->pid == p->id) {
	    DbFixation* fix = *pp;
	    erts_refc_dec(&tb->common.fixref,0);
	    --(fix->counter);
	    ASSERT(fix->counter >= 0);
	    if (fix->counter > 0) {
		break;
	    }
	    *pp = fix->next;
#ifdef ERTS_SMP
	    erts_smp_mtx_unlock(&tb->common.fixlock);
#endif
	    db_meta_lock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
	    db_erase_bag_exact2(meta_pid_to_fixed_tab,
				p->id, make_small(tb->common.slot));
	    db_meta_unlock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
	    erts_db_free(ERTS_ALC_T_DB_FIXATION,
			 tb, (void *) fix, sizeof(DbFixation));
	    ERTS_ETS_MISC_MEM_ADD(-sizeof(DbFixation));
	    goto unlocked;
	}
    }
#ifdef ERTS_SMP
    erts_smp_mtx_unlock(&tb->common.fixlock);
#endif
unlocked:

    if (!IS_FIXED(tb) && IS_HASH_TABLE(tb->common.status)
	&& erts_smp_atomic_read(&tb->hash.fixdel) != (long)NULL) {
#ifdef ERTS_SMP
	if (*kind_p == LCK_READ && tb->common.is_thread_safe) {
	    /* Must have write lock while purging pseudo-deleted (OTP-8166) */
	    db_ref(tb, LCK_WRITE); /* LCK_WRITE need it, but not LCK_READ */
	    erts_smp_rwmtx_runlock(&tb->common.rwlock);
	    erts_smp_rwmtx_rwlock(&tb->common.rwlock);
	    *kind_p = LCK_WRITE;
	    if (tb->common.status & DB_DELETE) return;
	}
#endif
	db_unfix_table_hash(&(tb->hash));
    }
}

/* Assume that tb is WRITE locked */
static void free_fixations_locked(DbTable *tb)
{
    DbFixation *fix;
    DbFixation *next_fix;

    fix = tb->common.fixations;
    while (fix != NULL) {
	next_fix = fix->next;
	db_meta_lock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
	db_erase_bag_exact2(meta_pid_to_fixed_tab,
			    fix->pid,
			    make_small(tb->common.slot));
	db_meta_unlock(meta_pid_to_fixed_tab, LCK_WRITE_REC);
	erts_db_free(ERTS_ALC_T_DB_FIXATION,
		     tb, (void *) fix, sizeof(DbFixation));
	ERTS_ETS_MISC_MEM_ADD(-sizeof(DbFixation));

	fix = next_fix;
    }
    tb->common.fixations = NULL;
}

static void set_heir(Process* me, DbTable* tb, Eterm heir, UWord heir_data)
{	
    tb->common.heir = heir;
    if (heir == am_none) {
	return;
    }
    if (heir == me->id) {
	tb->common.heir_started = me->started;
    }
    else {
	Process* heir_proc= erts_pid2proc_opt(me, ERTS_PROC_LOCK_MAIN, heir,
					      0, ERTS_P2P_FLG_SMP_INC_REFC);
	if (heir_proc != NULL) {
	    tb->common.heir_started = heir_proc->started;
	    erts_smp_proc_dec_refc(heir_proc);
	} else {
	    tb->common.heir = am_none;
	}
    }

    if (!is_immed(heir_data)) {
	DeclareTmpHeap(tmp,2,me);

	UseTmpHeap(2,me);
	/* Make a dummy 1-tuple around data to use db_get_term() */
	heir_data = (UWord) db_get_term(&tb->common, NULL, 0,
					TUPLE1(tmp,heir_data));
	UnUseTmpHeap(2,me);
	ASSERT(!is_immed(heir_data));
    }
    tb->common.heir_data = heir_data;
}

static void free_heir_data(DbTable* tb)
{
    if (tb->common.heir != am_none && !is_immed(tb->common.heir_data)) {
	DbTerm* p = (DbTerm*) tb->common.heir_data;
	db_free_term_data(p);
	erts_db_free(ERTS_ALC_T_DB_TERM, tb, (void *)p,
		     sizeof(DbTerm) + (p->size-1)*sizeof(Eterm));
    }
    #ifdef DEBUG
    tb->common.heir_data = am_undefined;
    #endif
}

static BIF_RETTYPE ets_delete_trap(Process *p, Eterm cont)
{
    int trap;
    Eterm* ptr = big_val(cont);
    DbTable *tb = *((DbTable **) (UWord) (ptr + 1));

#if HALFWORD_HEAP
    ASSERT(*ptr == make_pos_bignum_header(2));
#else
    ASSERT(*ptr == make_pos_bignum_header(1));
#endif
    db_lock(tb, LCK_WRITE);
    trap = free_table_cont(p, tb, 0, 1);
    db_unlock(tb, LCK_WRITE);

    if (trap) {
	BIF_TRAP1(&ets_delete_continue_exp, p, cont);
    }
    else {
	BIF_RET(am_true);
    }
}


/*
 * free_table_cont() returns 0 when done and !0 when more work is needed.
 */
static int free_table_cont(Process *p,
			   DbTable *tb,
			   int first,
			   int clean_meta_tab)
{
    Eterm result;
    erts_smp_rwmtx_t *mmtl;

#ifdef HARDDEBUG
    if (!first) {
	erts_fprintf(stderr,"ets: free_table_cont %T (continue)\r\n",
		     tb->common.id);
    }
#endif

    result = tb->common.meth->db_free_table_continue(tb);

    if (result == 0) {
#ifdef HARDDEBUG
	erts_fprintf(stderr,"ets: free_table_cont %T (continue begin)\r\n",
		     tb->common.id);
#endif
	/* More work to be done. Let other processes work and call us again. */
	BUMP_ALL_REDS(p);
	return !0;
    }
    else {
#ifdef HARDDEBUG
	erts_fprintf(stderr,"ets: free_table_cont %T (continue end)\r\n",
		     tb->common.id);
#endif
	/* Completely done - we will not get called again. */
	mmtl = get_meta_main_tab_lock(tb->common.slot);
#ifdef ERTS_SMP
	if (erts_smp_rwmtx_tryrwlock(mmtl) == EBUSY) {
	    /*
	     * We keep our increased refc over this op in order to
	     * prevent the table from disapearing.
	     */
	    erts_smp_rwmtx_rwunlock(&tb->common.rwlock);
	    erts_smp_rwmtx_rwlock(mmtl);
	    erts_smp_rwmtx_rwlock(&tb->common.rwlock);
	}
#endif
	free_slot(tb->common.slot);
	erts_smp_rwmtx_rwunlock(mmtl);

	if (clean_meta_tab) {
	    db_meta_lock(meta_pid_to_tab, LCK_WRITE_REC);
	    db_erase_bag_exact2(meta_pid_to_tab,tb->common.owner,
				make_small(tb->common.slot));
	    db_meta_unlock(meta_pid_to_tab, LCK_WRITE_REC);
	}
	db_unref(tb, LCK_NONE);
	BUMP_REDS(p, 100);
	return 0;
    }
}

static Eterm table_info(Process* p, DbTable* tb, Eterm What)
{
    Eterm ret = THE_NON_VALUE;

    if (What == am_size) {
	ret = make_small(erts_smp_atomic_read(&tb->common.nitems));
    } else if (What == am_type) {
	if (tb->common.status & DB_SET)  {
	    ret = am_set;
	} else if (tb->common.status & DB_DUPLICATE_BAG) {
	    ret = am_duplicate_bag;
	} else if (tb->common.status & DB_ORDERED_SET) {
	    ret = am_ordered_set;
	} else { /*TT*/
	    ASSERT(tb->common.status & DB_BAG);
	    ret = am_bag;
	}
    } else if (What == am_memory) {
	Uint words = (Uint) ((erts_smp_atomic_read(&tb->common.memory_size)
			      + sizeof(Uint)
			      - 1)
			     / sizeof(Uint));
	ret = erts_make_integer(words, p);
    } else if (What == am_owner) {
	ret = tb->common.owner;
    } else if (What == am_heir) {
	ret = tb->common.heir;
    } else if (What == am_protection) {
	if (tb->common.status & DB_PRIVATE) 
	    ret = am_private;
	else if (tb->common.status & DB_PROTECTED)
	    ret = am_protected;
	else if (tb->common.status & DB_PUBLIC)
	    ret = am_public;
    } else if (What == am_name) {
	ret = tb->common.the_name;
    } else if (What == am_keypos) {
	ret = make_small(tb->common.keypos);
    } else if (What == am_node) {
	ret = erts_this_dist_entry->sysname;
    } else if (What == am_named_table) {
	ret = is_atom(tb->common.id) ? am_true : am_false;
    /*
     * For debugging purposes
     */
    } else if (What == am_data) { 
	print_table(ERTS_PRINT_STDOUT, NULL, 1, tb);
	ret = am_true;
    } else if (What == am_atom_put("fixed",5)) { 
	if (IS_FIXED(tb))
	    ret = am_true;
	else
	    ret = am_false;
    } else if (What == am_atom_put("kept_objects",12)) {
	ret = make_small(IS_HASH_TABLE(tb->common.status)
			 ? db_kept_items_hash(&tb->hash) : 0);
    } else if (What == am_atom_put("safe_fixed",10)) {
#ifdef ERTS_SMP
	erts_smp_mtx_lock(&tb->common.fixlock);
#endif
	if (IS_FIXED(tb)) {
	    Uint need;
	    Eterm *hp;
	    Eterm tpl, lst;
	    DbFixation *fix;
	    need = 7;
	    for (fix = tb->common.fixations; fix != NULL; fix = fix->next) {
		need += 5;
	    }
	    hp = HAlloc(p, need);
	    lst = NIL;
	    for (fix = tb->common.fixations; fix != NULL; fix = fix->next) {
		tpl = TUPLE2(hp,fix->pid,make_small(fix->counter));
		hp += 3;
		lst = CONS(hp,tpl,lst);
		hp += 2;
	    }
	    tpl = TUPLE3(hp,
			 make_small(tb->common.megasec),
			 make_small(tb->common.sec),
			 make_small(tb->common.microsec));
	    hp += 4;
	    ret = TUPLE2(hp, tpl, lst);
	} else {
	    ret = am_false;
	}
#ifdef ERTS_SMP
	erts_smp_mtx_unlock(&tb->common.fixlock);
#endif
    } else if (What == am_atom_put("stats",5)) {
	if (IS_HASH_TABLE(tb->common.status)) {
	    FloatDef f;
	    DbHashStats stats;
	    Eterm avg, std_dev_real, std_dev_exp;
	    Eterm* hp;

	    db_calc_stats_hash(&tb->hash, &stats);
	    hp = HAlloc(p, 1 + 6 + FLOAT_SIZE_OBJECT*3);
	    f.fd = stats.avg_chain_len;
	    avg = make_float(hp);
	    PUT_DOUBLE(f, hp);
	    hp += FLOAT_SIZE_OBJECT;

	    f.fd = stats.std_dev_chain_len;
	    std_dev_real = make_float(hp);
	    PUT_DOUBLE(f, hp);
	    hp += FLOAT_SIZE_OBJECT;
	    
	    f.fd = stats.std_dev_expected;
	    std_dev_exp = make_float(hp);
	    PUT_DOUBLE(f, hp);
	    hp += FLOAT_SIZE_OBJECT;
	    ret = TUPLE6(hp, make_small(erts_smp_atomic_read(&tb->hash.nactive)),
			 avg, std_dev_real, std_dev_exp,
			 make_small(stats.min_chain_len),
			 make_small(stats.max_chain_len));
	}
	else {
	    ret = am_false;
	}
    }
    return ret;
}

static void print_table(int to, void *to_arg, int show,  DbTable* tb)
{
    erts_print(to, to_arg, "Table: %T\n", tb->common.id);
    erts_print(to, to_arg, "Name: %T\n", tb->common.the_name);

    tb->common.meth->db_print(to, to_arg, show, tb);

    erts_print(to, to_arg, "Objects: %d\n", (int)erts_smp_atomic_read(&tb->common.nitems));
    erts_print(to, to_arg, "Words: %bpu\n",
	       (Uint) ((erts_smp_atomic_read(&tb->common.memory_size)
			+ sizeof(Uint)
			- 1)
		       / sizeof(Uint)));
}

void db_info(int to, void *to_arg, int show)    /* Called by break handler */
{
    int i;
    for (i=0; i < db_max_tabs; i++) 
	if (IS_SLOT_ALIVE(i)) {
	    erts_print(to, to_arg, "=ets:%T\n", meta_main_tab[i].u.tb->common.owner);
	    erts_print(to, to_arg, "Slot: %d\n", i);
	    print_table(to, to_arg, show, meta_main_tab[i].u.tb);
	}
#ifdef DEBUG
    erts_print(to, to_arg, "=internal_ets: Process to table index\n");
    print_table(to, to_arg, show, meta_pid_to_tab);
    erts_print(to, to_arg, "=internal_ets: Process to fixation index\n");
    print_table(to, to_arg, show, meta_pid_to_fixed_tab);
#endif
}

Uint
erts_get_ets_misc_mem_size(void)
{
    /* Memory not allocated in ets_alloc */
    return (Uint) erts_smp_atomic_read(&erts_ets_misc_mem_size);
}

/* SMP Note: May only be used when system is locked */
void
erts_db_foreach_table(void (*func)(DbTable *, void *), void *arg)
{
    int i, j;
    j = 0;
    for(i = 0; (i < db_max_tabs && j < meta_main_tab_cnt); i++) {
	if (IS_SLOT_ALIVE(i)) {
	    j++;
	    (*func)(meta_main_tab[i].u.tb, arg);
	}
    }
    ASSERT(j == meta_main_tab_cnt);
}

/* SMP Note: May only be used when system is locked */
void
erts_db_foreach_offheap(DbTable *tb,
			void (*func)(ErlOffHeap *, void *),
			void *arg)
{
    tb->common.meth->db_foreach_offheap(tb, func, arg);
}

/*
 * For testing of meta tables only.
 *
 * Given a name atom (as returned from ets:new/2), return a list of 'cnt'
 * number of other names that will hash to the same bucket in meta_name_tab.
 *
 * WARNING: Will bloat the atom table!
 */
Eterm
erts_ets_colliding_names(Process* p, Eterm name, Uint cnt)
{
    Eterm list = NIL;
    Eterm* hp = HAlloc(p,cnt*2);
    Uint index = atom_val(name) & meta_name_tab_mask;

    while (cnt) {
        if (index != atom_val(name)) {
            while (index >= atom_table_size()) {
                char tmp[20];
                erts_snprintf(tmp, sizeof(tmp), "am%x", atom_table_size());
                am_atom_put(tmp,strlen(tmp));
            }
            list = CONS(hp, make_atom(index), list);
            hp += 2;
            --cnt;
        }
        index += meta_name_tab_mask + 1;
    }
    return list;
}


#ifdef HARDDEBUG   /* Here comes some debug functions */

void db_check_tables(void)
{
#ifdef ERTS_SMP
    return;
#else
    int i;

    for (i = 0; i < db_max_tabs; i++) {
	if (IS_SLOT_ALIVE(i)) {
	    DbTable* tb = meta_main_tab[i].t; 
	    tb->common.meth->db_check_table(tb);
	}
    }
#endif
}

#endif /* HARDDEBUG */
