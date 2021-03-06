#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "gwion.h"
#include "operator.h"
#include "traverse.h"
#include "vm.h"
#include "parse.h"

ANN static struct Env_Scope_ *new_envscope(MemPool p) {
  struct Env_Scope_ *a = mp_calloc(p, Env_Scope);
  vector_init(&a->breaks);
  vector_init(&a->conts);
  vector_init(&a->class_stack);
  vector_init(&a->nspc_stack);
  vector_init(&a->known_ctx);
  return a;
}

Env new_env(MemPool p) {
  const Env env = (Env)xmalloc(sizeof(struct Env_));
  env->global_nspc = new_nspc(p, "global_nspc");
  env->context = NULL;
  env->scope = new_envscope(p);
  env_reset(env);
  return env;
}

ANN void env_reset(const Env env) {
  vector_clear(&env->scope->breaks);
  vector_clear(&env->scope->conts);
  vector_clear(&env->scope->nspc_stack);
  vector_add(&env->scope->nspc_stack, (vtype)env->global_nspc);
  vector_clear(&env->scope->class_stack);
  vector_add(&env->scope->class_stack, (vtype)NULL);
  env->curr = env->global_nspc;
  env->class_def = NULL;
  env->func = NULL;
  env->scope->depth = 0;
}

ANN void release_ctx(struct Env_Scope_ *a, struct Gwion_ *gwion) {
  const m_uint size = vector_size(&a->known_ctx);
  for(m_uint i = size + 1; --i;)
    REM_REF((Context)vector_at(&a->known_ctx, i - 1), gwion);
}

ANN static void free_env_scope(struct Env_Scope_  *a, Gwion gwion) {
  release_ctx(a, gwion);
  vector_release(&a->known_ctx);
  vector_release(&a->nspc_stack);
  vector_release(&a->class_stack);
  vector_release(&a->breaks);
  vector_release(&a->conts);
  mp_free(gwion->mp, Env_Scope, a);
}

ANN void free_env(const Env a) {
  free_env_scope(a->scope, a->gwion);
  while(pop_global(a->gwion));
  xfree(a);
}

ANN2(1,3) m_uint env_push(const Env env, const Type type, const Nspc nspc) {
  const m_uint scope = env->scope->depth;
  vector_add(&env->scope->class_stack, (vtype)env->class_def);
  env->class_def = type;
  vector_add(&env->scope->nspc_stack, (vtype)env->curr);
  env->curr = nspc;
  env->scope->depth = 0;
  return scope;
}

ANN void env_pop(const Env env, const m_uint scope) {
  env->class_def = (Type)vector_pop(&env->scope->class_stack);
  env->curr = (Nspc)vector_pop(&env->scope->nspc_stack);
  env->scope->depth = scope;
}

ANN void env_add_type(const Env env, const Type type) {
  const Type v_type = type_copy(env->gwion->mp, env->gwion->type[et_class]);
  ADD_REF(v_type);
  v_type->e->d.base_type = type;
  SET_FLAG(type, builtin);
  const Symbol sym = insert_symbol(type->name);
  nspc_add_type_front(env->curr, sym, type);
  const Value v = new_value(env->gwion->mp, v_type, s_name(sym));
  SET_FLAG(v, valid | ae_flag_const | ae_flag_global | ae_flag_builtin);
  nspc_add_value(env->curr, insert_symbol(type->name), v);
  v->from->owner = type->e->owner = env->curr;
  v->from->owner_class = type->e->owner_class = env->class_def; // t owner_class ?
  type->xid = ++env->scope->type_xid;
}

ANN m_bool type_engine_check_prog(const Env env, const Ast ast) {
  const Context ctx = new_context(env->gwion->mp, ast, env->name);
  env_reset(env);
  load_context(ctx, env);
  const m_bool ret = traverse_ast(env, ast);
  if(ret > 0) //{
    nspc_commit(env->curr);
  if(ret > 0 || env->context->global)
    vector_add(&env->scope->known_ctx, (vtype)ctx);
  else //nspc_rollback(env->global_nspc);
    REM_REF(ctx, env->gwion);
  unload_context(ctx, env);
  return ret;
}
