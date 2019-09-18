#include <string.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "type.h"
#include "value.h"
#include "func.h"
#include "instr.h"
#include "object.h"
#include "traverse.h"
#include "template.h"
#include "optim.h"
#include "gwion.h"
#include "operator.h"
#include "import.h"
#include "parse.h"
#include "nspc.h"
#include "match.h"
#include "cpy_ast.h"
#include "tuple.h"

ANN static Type   check_exp(const Env env, Exp exp);
ANN static m_bool check_stmt_list(const Env env, Stmt_List list);
ANN m_bool check_class_def(const Env env, const Class_Def class_def);

ANN m_bool check_exp_array_subscripts(Env env, Exp exp) {
  CHECK_OB(check_exp(env, exp))
  do if(isa(exp->type, t_int) < 0)
      ERR_B(exp->pos, _("incompatible array subscript type '%s' ..."), exp->type->name)
  while((exp = exp->next));
  return GW_OK;
}

ANN static inline m_bool check_exp_decl_parent(const Env env, const Var_Decl var) {
  const Value value = find_value(env->class_def->e->parent, var->xid);
  if(value)
    ERR_B(var->pos,
          _("in class '%s': '%s' has already been defined in parent class '%s' ..."),
          env->class_def->name, s_name(var->xid),
          value->owner_class ? value->owner_class->name : "?")
  return GW_OK;
}

#define describe_check_decl(a, b)                                 \
ANN static inline void decl_##a(const Env env, const Value v) { \
  const Nspc nspc = env->curr;\
  SET_FLAG(v, a);                                                 \
  v->offset = nspc->info->b;                                      \
  nspc->info->b += v->type->size;                                 \
}
describe_check_decl(member, offset)
describe_check_decl(static, class_data_size)

ANN static m_bool check_fptr_decl(const Env env, const Var_Decl var) {
  const Value v    = var->value;
  const Func  func = v->type->e->d.func;
  const Type type = func->value_ref->owner_class;
  if(!env->class_def) {
    if(!type || GET_FLAG(func, global))
      return GW_OK;
    ERR_B(var->pos, _("can't use non public typedef at global scope."))
  }
  if(type && isa(type, env->class_def) < 0 && !GET_FLAG(func, global))
    ERR_B(var->pos, _("can't use non global fptr of other class."))
  if(GET_FLAG(func, member) && GET_FLAG(v, static))
      ERR_B(var->pos, _("can't use static variables for member function."))
  return GW_OK;
}

ANN Type check_td(const Env env, Type_Decl *td) {
  CHECK_BO(scan1_exp(env, td->exp))
  CHECK_BO(scan2_exp(env, td->exp))
  CHECK_OO(check_exp(env, td->exp))
  const Type t = actual_type(td->exp->type);
  if(!t || (isa(td->exp->type, t_class) < 0 && t == t_class))
    ERR_O(td->exp->pos, _("Expression must be of type '%s', not '%s'\n"
      "maybe you meant typeof(Expression)"), t_class->name, td->exp->type->name);
  m_uint depth;
  td->xid = str2list(env, t->name, &depth);

  if(depth) {
    Exp base = new_exp_prim_int(env->gwion->mp, 0, new_loc(env->gwion->mp, __LINE__)), e = base;
    for(m_uint i = 0; i < depth - 1; ++i) {
      e = (e->next = new_exp_prim_int(env->gwion->mp, 0, new_loc(env->gwion->mp, __LINE__)));
    }
    td->array = new_array_sub(env->gwion->mp, base);
  }
  return t;
}

ANN static inline void clear_decl(const Env env, const Exp_Decl *decl) {
  Var_Decl_List list = decl->list;
  do scope_add(env->curr->info->value, (vtype)list->self->xid, (vtype)NULL);
  while((list = list->next));
}

ANN static Type no_xid(const Env env, const Exp_Decl *decl) {
  DECL_OO(const Type, t, = check_td(env, decl->td))
  ((Exp_Decl*)decl)->type = NULL;
  clear_decl(env, decl);
  CHECK_BO(traverse_decl(env, decl))
  return decl->type;
}

ANN Type check_exp_decl(const Env env, const Exp_Decl* decl) {
  Var_Decl_List list = decl->list;
  if(!decl->td->xid)
    return no_xid(env, decl);
  if(decl->td->xid->xid == insert_symbol("auto")) { // should be better
    clear_decl(env, decl);
    CHECK_BO(scan1_exp(env, exp_self(decl)))
    CHECK_BO(scan2_exp(env, exp_self(decl)))
    if(isa(decl->type, t_class) > 0) {
      do {
        list->self->value->type = t_auto;
      } while((list = list->next));
      ((Exp_Decl*)decl)->type = t_auto;
    }
    if(decl->type == t_auto)
      ERR_O(td_pos(decl->td), _("can't infer type."));
  }
  if(!decl->type)
      ERR_O(td_pos(decl->td), _("can't infer type."));
{
  const Type t = get_type(decl->type);
  if(GET_FLAG(t, template) && !GET_FLAG(t, check))
    CHECK_BO(check_cdef(env, t->e->def))
}
  const m_bool global = GET_FLAG(decl->td, global);
  const m_uint scope = !global ? env->scope->depth : env_push_global(env);
  do {
    const Var_Decl var = list->self;
    const Value v = var->value;
    if(env->class_def && !env->scope->depth && env->class_def->e->parent)
      CHECK_BO(check_exp_decl_parent(env, var))
    if(var->array && var->array->exp)
      CHECK_BO(check_exp_array_subscripts(env, var->array->exp))
    if(env->class_def)  {
      if(GET_FLAG(decl->td, member)) {
        decl_member(env, v);
        if(isa(env->class_def, t_object)  > 0)
          tuple_info(env, decl->td, var);
      } else if(GET_FLAG(decl->td, static))
        decl_static(env, v);
    }
    else if(global || (env->func && GET_FLAG(env->func->def, global)))
      SET_FLAG(v, abstract);
    if(isa(decl->type, t_fptr) > 0)
      CHECK_BO(check_fptr_decl(env, var))
    SET_FLAG(v, checked | ae_flag_used);
    nspc_add_value(env->curr, var->xid, v);
  } while((list = list->next));
  if(global)
    env_pop(env, scope);
  return decl->type;
}

ANN static m_bool prim_array_inner(const Env env, const Type t, Type type, const Exp e) {
  const Type common = find_common_anc(t, type);
  if(common)
    return GW_OK;
  else if(isa(t, t_int) > 0 && isa(type, t_float) > 0) {
      e->cast_to = type;
      return GW_OK;
  }
  ERR_B(e->pos, _("array init [...] contains incompatible types ..."))
}

ANN static inline Type prim_array_match(const Env env, Exp e) {
  const Type type = e->type;
  do CHECK_BO(prim_array_inner(env, e->type, type, e))
  while((e = e->next));
  return array_type(env, type->array_depth ? array_base(type) : type, type->array_depth + 1);
}

ANN static Type prim_array(const Env env, const Exp_Primary* primary) {
  const Array_Sub array = primary->d.array;
  const Exp e = array->exp;
  if(!e)
    ERR_O(exp_self(primary)->pos, _("must provide values/expressions for array [...]"))
  CHECK_OO(check_exp(env, e))
  return (array->type = prim_array_match(env, e));
}

ANN static inline m_bool not_from_owner_class(const Env env, const Type t,
      const Value v, const loc_t pos) {
  if(!v->owner_class || isa(t, v->owner_class) < 0) {
    ERR_B(pos,
        _("'%s' from owner namespace '%s' used in '%s'."),
            v->name, v->owner ? v->owner->name : "?", t->name)
  }
  return GW_OK;
}

ANN static Value check_non_res_value(const Env env, const Exp_Primary* primary) {
  const Value value = nspc_lookup_value1(env->curr, primary->d.var);
  if(env->class_def) {
    const Value v = value ? value : find_value(env->class_def, primary->d.var);
    if(v) {
      if(v->owner_class)
        CHECK_BO(not_from_owner_class(env, env->class_def, v, exp_self(primary)->pos))
      if(env->func && GET_FLAG(env->func->def, static) && GET_FLAG(v, member))
        ERR_O(exp_self(primary)->pos,
              _("non-static member '%s' used from static function."), s_name(primary->d.var))
    }
    return v;
  } else if(env->func && GET_FLAG(env->func->def, global)) {
    if(!SAFE_FLAG(value, abstract) && !SAFE_FLAG(value, arg))
      ERR_O(exp_self(primary)->pos,
            _("non-global variable '%s' used from global function."), s_name(primary->d.var))
  }
  return value;
}

ANN static Type prim_id_non_res(const Env env, const Exp_Primary* primary) {
  const Value v = check_non_res_value(env, primary);
  if(!v || !GET_FLAG(v, checked)) {
    env_err(env, exp_self(primary)->pos,
          _("variable %s not legit at this point."), s_name(primary->d.var));
    if(v && v->owner_class)
      did_you_mean_type(v->owner_class, s_name(primary->d.var));
    else
      did_you_mean_nspc(v ? v->owner : env->curr, s_name(primary->d.var));
    return NULL;
  }
  if(env->func && !GET_FLAG(v, const) && v->owner)
    UNSET_FLAG(env->func, pure);
  SET_FLAG(v, used);
  ((Exp_Primary*)primary)->value = v;
  if(GET_FLAG(v, const) || !strcmp(s_name(primary->d.var), "maybe"))
    exp_self(primary)->meta = ae_meta_value;
  return v->type;
}

ANN static Type check_exp_prim_this(const Env env, const Exp_Primary* primary) {
  if(!env->class_def)
    ERR_O(exp_self(primary)->pos, _("keyword 'this' can be used only inside class definition..."))
  if(env->func && !GET_FLAG(env->func, member))
    ERR_O(exp_self(primary)->pos, _("keyword 'this' cannot be used inside static functions..."))
  exp_self(primary)->meta = ae_meta_value;
  return env->class_def;
}


ANN static inline Value prim_str_value(const Env env, const Symbol sym) {
  const Value v = nspc_lookup_value0(env->global_nspc, sym);
  if(v)
    return v;
  const Value value = new_value(env->gwion->mp, t_string, s_name(sym));
  map_set(&env->global_nspc->info->value->map, (vtype)sym, (vtype)value);
  return value;
}

ANN static Type prim_str(const Env env, Exp_Primary *const prim) {
  if(!prim->value) {
    const m_str str = prim->d.str;
    char c[strlen(str) + 8];
    sprintf(c, "%s:string", str);
    prim->value = prim_str_value(env, insert_symbol(c));
  }
  return t_string;
}

ANN static Type prim_id(const Env env, Exp_Primary* primary) {
  const m_str str = s_name(primary->d.var);
  if(!strcmp(str, "this"))
    return check_exp_prim_this(env, primary);
  else if(!strcmp(str, "__func__")) {
    primary->primary_type = ae_primary_str;
    primary->d.str = env->func ? env->func->name : env->class_def ?
      env->class_def->name : env->name;
    return prim_str(env, primary);
  } else
    return prim_id_non_res(env, primary);
}

ANN static m_bool vec_value(const Env env, Exp e, const m_str s) {
  int count = 1;
  CHECK_OB(check_exp(env, e))
  do {
    const Type t = e->type;
    if(isa(t, t_float) < 0) {
      if(isa(t, t_int) > 0)
        e->cast_to = t_float;
      else
        ERR_B(e->pos, _("invalid type '%s' in %s value #%d...\n"
              "    (must be of type 'int' or 'float')"), t->name, s, count)
    }
    ++count;
  } while((e = e->next));
  return GW_OK;
}

struct VecInfo {
  Type   t;
  m_str  s;
  m_uint n;
};

ANN static void vec_info(const ae_prim_t t, struct VecInfo* v) {
  if(t == ae_primary_complex) {
    v->s = "complex";
    v->t = t_complex;
    v->n = 2;
  } else if(t == ae_primary_vec) {
    v->t = v->n == 4 ? t_vec4 : t_vec3;
    v->n = 4;
    v->s = "vector";
  } else {
    v->s = "polar";
    v->t = t_polar;
    v->n = 2;
  }
}

ANN static Type prim_vec(const Env env, const Exp_Primary* primary) {
  const Vec * vec = &primary->d.vec;
  const ae_prim_t t = primary->primary_type;
  struct VecInfo info = { .n=vec->dim };
  vec_info(t, &info);
  if(vec->dim > info.n)
    ERR_O(vec->exp->pos, _("extraneous component of %s value..."), info.s)
  CHECK_BO(vec_value(env, vec->exp, info.s))
  return info.t;
}

ANN static Type prim_gack(const Env env, const Exp_Primary * primary) {
  if(env->func)
    UNSET_FLAG(env->func, pure);
  CHECK_OO((check_exp(env, primary->d.exp)))
  return t_gack;
}

ANN static Type prim_tuple(const Env env, const Exp_Primary * primary) {
  CHECK_OO(check_exp(env, primary->d.tuple.exp))
  Exp e = primary->d.tuple.exp;
  struct Vector_ v;
  vector_init(&v);
  do {
    vector_add(&v, (m_uint)e->type);
  } while((e = e->next));
  const Type ret = tuple_type(env, &v, exp_self(primary)->pos);
  vector_release(&v);
  return ret;
}

#define describe_prim_xxx(name, type) \
ANN static Type prim_##name(const Env env NUSED, const Exp_Primary * primary NUSED) {\
  return type; \
}
describe_prim_xxx(num, t_int)
describe_prim_xxx(float, t_float)
describe_prim_xxx(nil, t_void)
describe_prim_xxx(unpack, t_tuple)

typedef Type (*_type_func)(const Env, const void*);
static const _type_func prim_func[] = {
  (_type_func)prim_id,    (_type_func)prim_num,   (_type_func)prim_float, (_type_func)prim_str,
  (_type_func)prim_array, (_type_func)prim_gack,  (_type_func)prim_vec,   (_type_func)prim_vec,
  (_type_func)prim_vec,   (_type_func)prim_tuple, (_type_func)prim_unpack,
  (_type_func)prim_num,   (_type_func)prim_nil,
};

ANN static Type check_exp_primary(const Env env, const Exp_Primary* primary) {
  return exp_self(primary)->type = prim_func[primary->primary_type](env, primary);
}

ANN static Type at_depth(const Env env, const Array_Sub array);
ANN static Type tuple_depth(const Env env, const Array_Sub array) {
  if(array->exp->exp_type != ae_exp_primary ||
     array->exp->d.exp_primary.primary_type != ae_primary_num)
     ERR_O(array->exp->pos, _("tuple subscripts must be litteral"))
  const m_uint idx = array->exp->d.exp_primary.d.num;
  if(idx >= vector_size(&array->type->e->tuple->types))
     ERR_O(array->exp->pos, _("tuple subscripts too big"))
  const Type type = (Type)vector_at(&array->type->e->tuple->types, idx);
  if(type == t_undefined)
     ERR_O(array->exp->pos, _("tuple subscripts is undefined at index %lu"),
         idx)
  if(!array->exp->next)
    return type;
  struct Array_Sub_ next = { array->exp->next, type, array->depth - 1 };
  return at_depth(env, &next);
}

ANN static Type partial_depth(const Env env, const Array_Sub array) {
  const Exp curr = take_exp(array->exp, array->type->array_depth);
  if(!curr->next || !array_base(array->type))
    ERR_O(array->exp->pos, _("array subscripts (%i) exceeds defined dimension (%i)"),
        array->depth, get_depth(array->type))
  struct Array_Sub_ next = { curr->next, array_base(array->type), array->depth - array->type->array_depth };
  return at_depth(env, &next);
}

ANN static Type at_depth(const Env env, const Array_Sub array) {
  const Type t = array->type;
  const m_uint depth = array->depth;
  if(GET_FLAG(t, typedef)) {
    struct Array_Sub_ next = { array->exp, t->e->parent, depth };
    return at_depth(env, &next);
  }
  if(t->array_depth >= depth)
    return array_type(env, array_base(array->type), t->array_depth - depth);
  return (isa(t, t_tuple) < 0 ? partial_depth : tuple_depth)(env, array);
}

static inline m_bool index_is_int(const Env env, Exp e, m_uint *depth) {
  do if(isa(e->type, t_int) < 0)
    ERR_B(e->pos, _("array index %i must be of type 'int', not '%s'"),
        *depth, e->type->name)
  while(++(*depth) && (e = e->next));
  return GW_OK;
}

static m_bool array_access_valid(const Env env, const Exp_Array* array) {
  m_uint depth = 0;
  CHECK_BB(index_is_int(env, array->array->exp, &depth))
  if(depth != array->array->depth)
    ERR_B(exp_self(array)->pos, _("invalid array acces expression."))
  return GW_OK;
}

static ANN Type check_exp_array(const Env env, const Exp_Array* array) {
  CHECK_OO((array->array->type = check_exp(env, array->base)))
  CHECK_OO(check_exp(env, array->array->exp))
  CHECK_BO(array_access_valid(env, array))
  return at_depth(env, array->array);
}

ANN static Type_List mk_type_list(const Env env, const Type type) {
  struct Vector_ v;
  vector_init(&v);
  vector_add(&v, (vtype)insert_symbol(type->name));
  Nspc nspc = type->e->owner;
  while(nspc && nspc != env->curr && nspc != env->global_nspc) {
    const Type t = nspc_lookup_type0(nspc->parent, insert_symbol(nspc->name));
    vector_add(&v, (vtype)insert_symbol(t->name));
    nspc = nspc->parent;
  }
  ID_List id = NULL;
  for(m_uint i = vector_size(&v) + 1; --i;)
    id = prepend_id_list(env->gwion->mp, (Symbol)vector_at(&v, i - 1), id, new_loc(env->gwion->mp, __LINE__));
  vector_release(&v);
  assert(id);
  Type_Decl* td = new_type_decl(env->gwion->mp, id);
  return new_type_list(env->gwion->mp, td, NULL);
}

ANN static m_bool func_match_inner(const Env env, const Exp e, const Type t,
  const m_bool implicit, const m_bool specific) {
  const m_bool match = (specific ? e->type == t : isa(e->type, t) > 0) &&
    e->type->array_depth == t->array_depth &&
    array_base(e->type) == array_base(t);
    if(!match) {
      if(e->type == t_lambda && isa(t, t_fptr) > 0) {
        const Type owner = nspc_lookup_type1(t->e->owner->parent,
          insert_symbol(t->e->owner->name));
        return check_lambda(env, owner, &e->d.exp_lambda, t->e->d.func->def);
      }
      if(implicit) {
        const struct Implicit imp = { e, t, e->pos };
        struct Op_Import opi = { .op=insert_symbol("@implicit"), .lhs=e->type, .rhs=t, .data=(m_uint)&imp, .pos=e->pos };
        return op_check(env, &opi) ? GW_OK : GW_ERROR;
    }
  }
  return match ? 1 : -1;
}

ANN2(1,2) static Func find_func_match_actual(const Env env, Func func, const Exp args,
  const m_bool implicit, const m_bool specific) {
  do {
    Exp e = args;
    Arg_List e1 = func->def->base->args;
    while(e) {
      if(!e1) {
        if(GET_FLAG(func->def, variadic))
          return func;
        CHECK_OO(func->next);
        return find_func_match_actual(env, func->next, args, implicit, specific);
      }
      if(e1->type == t_undefined ||
            (func->def->base->tmpl && is_fptr(func->value_ref->type) > 0)) {
        if(SAFE_FLAG(func->value_ref->owner_class, template))
          CHECK_BO(template_push_types(env, func->value_ref->owner_class->e->def->base.tmpl))
          e1->type = known_type(env, e1->td);
        if(SAFE_FLAG(func->value_ref->owner_class, template))
          nspc_pop_type(env->gwion->mp, env->curr);
        CHECK_OO(e1->type)
      }
      if(func_match_inner(env, e, e1->type, implicit, specific) < 0)
        break;
      e = e->next;
      e1 = e1->next;
    }
    if(!e1)
      return func;
  } while((func = func->next));
  return NULL;
}

ANN2(1, 2) static Func find_func_match(const Env env, const Func up, const Exp exp) {
  Func func;
  const Exp args = (exp && isa(exp->type, t_void) < 0) ? exp : NULL;
  if((func = find_func_match_actual(env, up, args, 0, 1)) ||
     (func = find_func_match_actual(env, up, args, 1, 1)) ||
     (func = find_func_match_actual(env, up, args, 0, 0)) ||
     (func = find_func_match_actual(env, up, args, 1, 0)))
    return func;
  return NULL;
}

ANN static m_bool check_call(const Env env, const Exp_Call* exp) {
  ae_exp_t et = exp->func->exp_type;
  if(et != ae_exp_primary && et != ae_exp_dot && et != ae_exp_cast)
    ERR_B(exp->func->pos, _("invalid expression for function call."))
  CHECK_OB(check_exp(env, exp->func))
  if(exp->args)
    CHECK_OB(check_exp(env, exp->args))
  return GW_OK;
}

ANN static inline Value template_get_ready(const Env env, const Value v, const m_str tmpl, const m_uint i) {
  const Symbol sym = func_symbol(env, v->owner->name, v->name, tmpl, i);
  return v->owner_class ? find_value(v->owner_class, sym) :
      nspc_lookup_value1(v->owner, sym);
}

static Func ensure_tmpl(const Env env, const Func_Def fdef, const Exp_Call *exp) {
  const m_bool ret = traverse_func_def(env, fdef);
  if(ret > 0) {
    const Func f = fdef->base->func;
    const Func next = f->next;
    f->next = NULL;
    const Func func = find_func_match(env, f, exp->args);
    f->next = next;
    if(func) {
      SET_FLAG(func, checked | ae_flag_template);
      return func;
    }
  }
  return NULL;
}

ANN static m_bool check_func_args(const Env env, Arg_List arg_list) {
  do {
    const Var_Decl decl = arg_list->var_decl;
    const Value v = decl->value;
    if(arg_list->td && !arg_list->td->xid)
      arg_list->type = v->type = check_td(env, arg_list->td);
    if(isa(v->type, t_object) > 0 || isa(v->type, t_function) > 0)
      UNSET_FLAG(env->func, pure);
    CHECK_BB(already_defined(env, decl->xid, decl->pos))
    SET_FLAG(v, checked);
    nspc_add_value(env->curr, decl->xid, v);
  } while((arg_list = arg_list->next));
  return GW_OK;
}

ANN static Func _find_template_match(const Env env, const Value v, const Exp_Call* exp) {
CHECK_BO(check_call(env, exp))
  const Type_List types = exp->tmpl->call;
  Func m_func = NULL, former = env->func;
  DECL_OO(const m_str, tmpl_name, = tl2str(env, types))
  const m_uint scope = env_push(env, v->owner_class, v->owner);
  if(is_fptr(v->type)) {
  const Symbol sym = func_symbol(env, v->owner->name, v->name, tmpl_name, 0);
  const Value value = nspc_lookup_value1(v->owner, sym);
  Func_Def base = v->d.func_ref ? v->d.func_ref->def : exp->func->type->e->d.func->def;
  Func_Base *fbase = cpy_func_base(env->gwion->mp, base->base);
  fbase->xid = sym;
  fbase->tmpl->base = 0;
  fbase->tmpl->call = types;
  if(template_push_types(env, fbase->tmpl) > 0) {
    const Fptr_Def fptr = new_fptr_def(env->gwion->mp, fbase, base->flag);
    if(value) {
      fptr->type = actual_type(value->type);
      fptr->value = value;
    }
    if(traverse_fptr_def(env, fptr) > 0 &&
       (base->base->ret_type = known_type(env, base->base->td)) &&
       (!exp->args || !!check_exp(env, exp->args))) {
      m_func = find_func_match(env, fbase->func, exp->args);
      nspc_pop_type(env->gwion->mp, env->curr);
      if(!value && m_func) {
        map_set(&v->owner->info->type->map, (vtype)sym, (vtype)actual_type(m_func->value_ref->type));
      }
    } else if(!nspc_lookup_value1(v->owner, sym))
      free_fptr_def(env->gwion->mp, fptr); // ???? related
  }
  } else {
    for(m_uint i = 0; i < v->offset + 1; ++i) {
      const Value exists = template_get_ready(env, v, tmpl_name, i);
      if(exists) {
        if(env->func == exists->d.func_ref) {
          if(check_call(env, exp) < 0)
            continue;
          CHECK_OO(find_func_match(env, env->func, exp->args))
          m_func = env->func;
          break;
        }
        if((m_func = ensure_tmpl(env, exists->d.func_ref->def, exp)))
          break;
      } else {
        const Value value = template_get_ready(env, v, "template", i);
        if(!value)
          continue;
        const Func_Def fdef = (Func_Def)cpy_func_def(env->gwion->mp, value->d.func_ref->def);
        SET_FLAG(fdef, template);
        fdef->base->tmpl->call = types;
        fdef->base->tmpl->base = i;
        if((m_func = ensure_tmpl(env, fdef, exp)))
          break;
      }
    }
  }
  free_mstr(env->gwion->mp, tmpl_name);
  env_pop(env, scope);
  env->func = former;
  return m_func;
}

ANN Func find_template_match(const Env env, const Value value, const Exp_Call* exp) {
  Type t = value->owner_class;
  const Func f = _find_template_match(env, value, exp);
  if(f)
    return f;
  while(t) {
   Value v = nspc_lookup_value1(t->nspc, value->d.func_ref->def->base->xid);
   if(!v)
     goto next;
   const Func f = _find_template_match(env, v, exp);
   if(f)
     return f;
   next:
     t = t->e->parent;
  }
  assert(exp_self(exp));
  ERR_O(exp_self(exp)->pos, _("arguments do not match for template call"))
}

ANN static void print_current_args(Exp e) {
  gw_err(_("and not\n  "));
  do gw_err(" \033[32m%s\033[0m", e->type->name);
  while((e = e->next) && gw_err(","));
  gw_err("\n");
}

ANN static void print_arg(Arg_List e) {
  do gw_err(" \033[32m%s\033[0m \033[1m%s\033[0m", e->type ? e->type->name : NULL,
       e->var_decl->xid ? s_name(e->var_decl->xid)  : "");
  while((e = e->next) && gw_err(","));
}

ANN2(1) static void function_alternative(const Env env, const Type f, const Exp args, const loc_t pos){
  env_err(env, pos, _("argument type(s) do not match for function. should be :"));
  Func up = f->e->d.func;
  do {
    gw_err("(%s)  ", up->name);
    const Arg_List e = up->def->base->args;
    e ? print_arg(e) : (void)gw_err("\033[32mvoid\033[0m");
    gw_err("\n");
  } while((up = up->next));
  args ? print_current_args(args) : (void)gw_err(_("and not:\n  \033[32mvoid\033[0m\n"));
}

ANN static m_uint get_type_number(ID_List list) {
  m_uint type_number = 0;
  do ++type_number;
  while((list = list->next));
  return type_number;
}

ANN static Func get_template_func(const Env env, const Exp_Call* func, const Value v) {
  const Func f = find_template_match(env, v, func);
  if(f) {
    Tmpl* tmpl = new_tmpl_call(env->gwion->mp, func->tmpl->call);
    tmpl->list = v->d.func_ref ? v->d.func_ref->def->base->tmpl->list : func->func->type->e->d.func->def->base->tmpl->list;
    ((Exp_Call*)func)->tmpl = tmpl;
    return ((Exp_Call*)func)->m_func = f;
  }
  ((Exp_Call*)func)->tmpl = NULL;
  assert(exp_self(func));
  ERR_O(exp_self(func)->pos,
        _("function is template. automatic type guess not fully implemented yet.\n"
        "  please provide template types. eg: '<~type1, type2, ...~>'"))
}

ANN static Func predefined_func(const Env env, const Value v,
    Exp_Call *exp, const Tmpl *tm) {
  Tmpl tmpl = { .call=tm->call };
  exp->tmpl = &tmpl;
  DECL_OO(const Func, func, = get_template_func(env, exp, v))
  return v->d.func_ref = func;
}

ANN static Type check_exp_call_template(const Env env, Exp_Call *exp) {
  const Exp call = exp->func;
  const Exp args = exp->args;
  DECL_OO(const Value, value, = nspc_lookup_value1(call->type->e->owner, insert_symbol(call->type->name)))
  Tmpl *tm = value->d.func_ref ? value->d.func_ref->def->base->tmpl : call->type->e->d.func->def->base->tmpl;
  if(tm->call) {
    DECL_OO(const Func, func, = value->d.func_ref ?: predefined_func(env, value, exp, tm))
    if(!func->def->base->ret_type) { // template fptr
      const m_uint scope = env_push(env, value->owner_class, value->owner);
      CHECK_BO(traverse_func_def(env, func->def))
      env_pop(env, scope);
    }
    exp->m_func = func;
    return func->def->base->ret_type;
  }
  m_uint args_number = 0;
  const m_uint type_number = get_type_number(tm->list);
  Type_List tl[type_number];
  ID_List list = tm->list;
  while(list) {
    Arg_List arg = value->d.func_ref->def->base->args;
    Exp template_arg = args;
    while(arg && template_arg) {
      char path[id_list_len(arg->td->xid)];
      type_path(path, arg->td->xid);
      if(!strcmp(s_name(list->xid), path)) {
        tl[args_number] = mk_type_list(env, template_arg->type);
        if(args_number)
          tl[args_number - 1]->next = tl[args_number];
        ++args_number;
        break;
      }
      arg = arg->next;
      template_arg = template_arg->next;
    }
    list = list->next;
  }
  if(args_number < type_number)
    ERR_O(call->pos, _("not able to guess types for template call."))
  Tmpl tmpl = { .call=tl[0] };
  ((Exp_Call*)exp)->tmpl = &tmpl;
  DECL_OO(const Func,func, = get_template_func(env, exp, value))
  return func->def->base->ret_type;
}

ANN static m_bool check_exp_call1_check(const Env env, const Exp exp) {
  CHECK_OB(check_exp(env, exp))
  if(isa(exp->type, t_function) < 0)
    ERR_B(exp->pos, _("function call using a non-function value"))
  return GW_OK;
}

ANN static Type check_lambda_call(const Env env, const Exp_Call *exp) {
  if(exp->args)
    CHECK_OO(check_exp(env, exp->args))
  Exp_Lambda *l = &exp->func->d.exp_lambda;
  Arg_List arg = l->args;
  Exp e = exp->args;
  while(arg && e) {
    arg->type = e->type;
    arg = arg->next;
    e = e->next;
  }
  if(arg || e)
    ERR_O(exp_self(exp)->pos, _("argument number does not match for lambda"))
  l->def = new_func_def(env->gwion->mp, new_func_base(env->gwion->mp, NULL, l->name, l->args),
    l->code, 0, loc_cpy(env->gwion->mp, loc_cpy(env->gwion->mp, exp_self(exp)->pos)));
  CHECK_BO(traverse_func_def(env, l->def))
  if(env->class_def)
    SET_FLAG(l->def, member);
  ((Exp_Call*)exp)->m_func = l->def->base->func;
  return l->def->base->ret_type ?: (l->def->base->ret_type = t_void);
}

ANN Type check_exp_call1(const Env env, const Exp_Call *exp) {
  CHECK_BO(check_exp_call1_check(env, exp->func))
  if(exp->func->type == t_lambda)
    return check_lambda_call(env, exp);
  if(GET_FLAG(exp->func->type->e->d.func, ref)) {
    const Value value = exp->func->type->e->d.func->value_ref;
    if(value->owner_class)
      CHECK_BO(traverse_class_def(env, value->owner_class->e->def))
  }
  if(exp->args)
    CHECK_OO(check_exp(env, exp->args))
  if(GET_FLAG(exp->func->type, func))
    return check_exp_call_template(env, (Exp_Call*)exp);
  const Func func = find_func_match(env, exp->func->type->e->d.func, exp->args);
  if((exp_self(exp)->d.exp_call.m_func = func)) {
    exp->func->type = func->value_ref->type;
    return func->def->base->ret_type;
  }
  function_alternative(env, exp->func->type, exp->args, exp_self(exp)->pos);
  return NULL;
}

ANN static Type check_exp_binary(const Env env, const Exp_Binary* bin) {
  if(bin->lhs->exp_type == ae_exp_unary && bin->lhs->d.exp_unary.op == insert_symbol("fork") &&
       bin->rhs->exp_type == ae_exp_decl)
     bin->lhs->d.exp_unary.fork_ok = 1;
  CHECK_OO(check_exp(env, bin->lhs))
  if(bin->rhs->exp_type == ae_exp_decl && bin->rhs->d.exp_decl.type == t_auto)
    bin->rhs->type = bin->rhs->d.exp_decl.type = bin->lhs->type;
  CHECK_OO(check_exp(env, bin->rhs))
  struct Op_Import opi = { .op=bin->op, .lhs=bin->lhs->type,
    .rhs=bin->rhs->type, .data=(uintptr_t)bin, .pos=exp_self(bin)->pos };
  return op_check(env, &opi);
}

ANN static Type check_exp_cast(const Env env, const Exp_Cast* cast) {
  DECL_OO(const Type, t, = check_exp(env, cast->exp))
  CHECK_OO((exp_self(cast)->type = cast->td->xid ? known_type(env, cast->td) : check_td(env, cast->td)))
  struct Op_Import opi = { .op=insert_symbol("$"), .lhs=t, .rhs=exp_self(cast)->type, .data=(uintptr_t)cast, .pos=exp_self(cast)->pos };
  return op_check(env, &opi);
}

ANN static Type check_exp_post(const Env env, const Exp_Postfix* post) {
  struct Op_Import opi = { .op=post->op, .lhs=check_exp(env, post->exp), .data=(uintptr_t)post, .pos=exp_self(post)->pos };
  CHECK_OO(opi.lhs)
  const Type t = op_check(env, &opi);
  if(t && isa(t, t_object) < 0)
    exp_self(post)->meta = ae_meta_value;
  return t;
}

ANN static m_bool predefined_call(const Env env, const Type t, const loc_t pos) {
  const m_str str = tl2str(env, t->e->d.func->def->base->tmpl->call);
  env_err(env, pos, _("Type '%s' has '%s' as pre-defined types."),
      t->name, str);
  free_mstr(env->gwion->mp, str);
  if(GET_FLAG(t, typedef)) {
    loc_header(t->e->d.func->def->pos, env->name);
    gw_err(_("from definition:\n"));
    loc_err(t->e->d.func->def->pos, env->name);
  }
  return GW_ERROR;
}

ANN static Type check_exp_call(const Env env, Exp_Call* exp) {
  if(exp->tmpl) {
    CHECK_OO(check_exp(env, exp->func))
    const Type t = actual_type(!GET_FLAG(exp->func->type, nonnull) ?
       exp->func->type : exp->func->type->e->parent);
    const Value v = nspc_lookup_value1(t->e->owner, insert_symbol(t->name));
    if(!GET_FLAG(v, func) && !GET_FLAG(exp->func->type, func) )
      ERR_O(exp_self(exp)->pos, _("template call of non-function value."))
    if(!v->d.func_ref || !v->d.func_ref->def->base->tmpl)
      ERR_O(exp_self(exp)->pos, _("template call of non-template function."))
    if(t->e->d.func->def->base->tmpl->call) {
      if(env->func == t->e->d.func) {
        CHECK_OO(check_exp(env, exp->args))
        exp->m_func = env->func;
        return env->func->def->base->ret_type;
      }  else
      CHECK_BO(predefined_call(env, t, exp_self(exp)->pos))
    }
    const Func ret = find_template_match(env, v, exp);
    CHECK_OO((exp->m_func = ret))
    return ret->def->base->ret_type;
  }
  return check_exp_call1(env, exp);
}

ANN static Type check_exp_unary(const Env env, const Exp_Unary* unary) {
  struct Op_Import opi = { .op=unary->op, .rhs=unary->exp ? check_exp(env, unary->exp) : NULL,
    .data=(uintptr_t)unary, .pos=exp_self(unary)->pos };
  if(unary->exp && !opi.rhs)
    return NULL;
  return op_check(env, &opi);
}

ANN static Type check_exp_if(const Env env, const Exp_If* exp_if) {
  DECL_OO(const Type, cond, = check_exp(env, exp_if->cond))
  DECL_OO(const Type, if_exp, = (exp_if->if_exp ? check_exp(env, exp_if->if_exp) : cond))
  DECL_OO(const Type, else_exp, = check_exp(env, exp_if->else_exp))
  if(isa(cond, t_int) < 0 && isa(cond, t_float) < 0 && isa(cond, t_object) < 0)
    ERR_O(exp_self(exp_if)->pos,
          _("Invalid type '%s' in if expression condition."), cond->name)
  const Type ret = find_common_anc(if_exp, else_exp);
  if(!ret)
    ERR_O(exp_self(exp_if)->pos,
          _("incompatible types '%s' and '%s' in if expression..."),
          if_exp->name, else_exp->name)
  return ret;
}

ANN static Type check_exp_dot(const Env env, Exp_Dot* member) {
  const m_str str = s_name(member->xid);
  CHECK_OO((member->t_base = check_exp(env, member->base)))
  const m_bool base_static = isa(member->t_base, t_class) > 0;
  const Type the_base = base_static ? member->t_base->e->d.base_type : member->t_base;
  if(!the_base->nspc)
    ERR_O(member->base->pos,
          _("type '%s' does not have members - invalid use in dot expression of %s"),
          the_base->name, str)
  if(member->xid ==  insert_symbol("this") && base_static)
    ERR_O(exp_self(member)->pos,
          _("keyword 'this' must be associated with object instance..."))
  const Value value = find_value(the_base, member->xid);
  if(!value) {
    env_err(env, exp_self(member)->pos,
          _("class '%s' has no member '%s'"), the_base->name, str);
    if(member->t_base->nspc)
      did_you_mean_type(member->t_base, str);
    return NULL;
  }
  CHECK_BO(not_from_owner_class(env, the_base, value, exp_self(member)->pos))
  if(!env->class_def || isa(env->class_def, value->owner_class) < 0) {
    if(GET_FLAG(value, private))
      ERR_O(exp_self(member)->pos,
          _("can't access private '%s' outside of class..."), value->name)
    else if(GET_FLAG(value, protect))
      exp_self(member)->meta = ae_meta_protect;
  }
  if(base_static && GET_FLAG(value, member))
    ERR_O(exp_self(member)->pos,
          _("cannot access member '%s.%s' without object instance..."),
          the_base->name, str)
  if(GET_FLAG(value, const) || GET_FLAG(value, enum))
    exp_self(member)->meta = ae_meta_value;
  return value->type;
}

ANN m_bool check_type_def(const Env env, const Type_Def tdef) {
  return tdef->type->e->def ? check_class_def(env, tdef->type->e->def) : GW_OK;
}
ANN static Type check_exp_lambda(const Env env NUSED,
    const Exp_If* exp_if NUSED) { return t_lambda; }

ANN static Type check_exp_typeof(const Env env, const Exp_Typeof *exp) {
  DECL_OO(const Type, t, = check_exp(env, exp->exp))
  DECL_OO(Value, v, = nspc_lookup_value1(t->e->owner, insert_symbol(t->name)))
  return v->type;
}

static const _type_func exp_func[] = {
  (_type_func)check_exp_decl,    (_type_func)check_exp_binary, (_type_func)check_exp_unary,
  (_type_func)check_exp_primary, (_type_func)check_exp_cast,   (_type_func)check_exp_post,
  (_type_func)check_exp_call,    (_type_func)check_exp_array,  (_type_func)check_exp_if,
  (_type_func)check_exp_dot,     (_type_func)check_exp_lambda, (_type_func)check_exp_typeof
};

ANN static inline Type check_exp(const Env env, const Exp exp) {
  Exp curr = exp, next = NULL, prev = NULL;
  do {
    next = curr->next;
    CHECK_OO((curr->type = exp_func[curr->exp_type](env, &curr->d)))
    if(isa(curr->type, t_varloop) > 0 && (prev || next))
      ERR_O(exp->pos, _("Varloop must be the only expression"))
    if(env->func && isa(curr->type, t_lambda) < 0 && isa(curr->type, t_function) > 0 &&
        !GET_FLAG(curr->type->e->d.func, pure))
      UNSET_FLAG(env->func, pure);
  } while((prev = curr) && (curr = next));
  return exp->type;
}

ANN m_bool check_enum_def(const Env env, const Enum_Def edef) {
  if(env->class_def) {
    ID_List list = edef->list;
    do decl_static(env, nspc_lookup_value0(env->curr, list->xid));
    while((list = list->next));
  }
  return GW_OK;
}

ANN static m_bool check_stmt_code(const Env env, const Stmt_Code stmt) {
  if(stmt->stmt_list) { RET_NSPC(check_stmt_list(env, stmt->stmt_list)) }
  return GW_OK;
}

ANN static m_bool check_flow(const Env env, const Exp exp, const m_str orig) {
  if(isa(exp->type, t_object) > 0 || isa(exp->type, t_int) > 0 || isa(exp->type, t_float) > 0 ||
     isa(exp->type, t_dur) > 0 || isa(exp->type, t_time)  > 0)
    return GW_OK;
  ERR_B(exp->pos, _("invalid type '%s' (in '%s' condition)"), exp->type->name, orig)
}

ANN static m_bool check_breaks(const Env env, const Stmt a, const Stmt b) {
  vector_add(&env->scope->breaks, (vtype)a);
  RET_NSPC(check_stmt(env, b))
  vector_pop(&env->scope->breaks);
  return ret;
}

ANN static m_bool check_conts(const Env env, const Stmt a, const Stmt b) {
  vector_add(&env->scope->conts, (vtype)a);
  CHECK_BB(check_breaks(env, a, b))
  vector_pop(&env->scope->conts);
  return GW_OK;
}

ANN static inline m_bool for_empty(const Env env, const Stmt_For stmt) {
  if(!stmt->c2 || !stmt->c2->d.stmt_exp.val)
    ERR_B(stmt_self(stmt)->pos, _("empty for loop condition..."
          "...(note: explicitly use 'true' if it's the intent)"
          "...(e.g., 'for(; true;){ /*...*/ }')"))
  return GW_OK;
}

ANN static m_bool do_stmt_auto(const Env env, const Stmt_Auto stmt) {
  DECL_OB(Type, t, = check_exp(env, stmt->exp))
  Type ptr = array_base(t);
  const m_uint depth = t->array_depth - 1;
  if(GET_FLAG(t, typedef))
    t = t->e->parent;
  if(!ptr || isa(t, t_array) < 0)
    ERR_B(stmt_self(stmt)->pos, _("type '%s' is not array.\n"
          " This is not allowed in auto loop"), stmt->exp->type->name)
  if(stmt->is_ptr) {
    struct ID_List_   id0, id;
    struct Type_List_ tl;
    struct Array_Sub_ array;
    Type_Decl td0, td;
    memset(&id0, 0, sizeof(struct ID_List_));
    memset(&id, 0, sizeof(struct ID_List_));
    memset(&tl, 0, sizeof(struct Type_List_));
    memset(&array, 0, sizeof(struct Array_Sub_));
    memset(&td0, 0, sizeof(Type_Decl));
    memset(&td, 0, sizeof(Type_Decl));
    id.xid   = insert_symbol("Ptr");
    id0.xid  = insert_symbol(ptr->name);
    td0.xid  = &id0;
    td.xid   = &id;
    tl.td    = &td0;
    td.types = &tl;
    if(depth) {
      array.depth = depth;
      td.array = &array;
    }
//    ptr = type_decl_resolve(env, &td); exit(3);
    ptr = known_type(env, &td);
    if(!GET_FLAG(ptr, checked))
      check_class_def(env, ptr->e->def);
//      traverse_class_def(env, ptr->e->def);
  }
  t = depth ? array_type(env, ptr, depth) : ptr;
  stmt->v = new_value(env->gwion->mp, t, s_name(stmt->sym));
  SET_FLAG(stmt->v, checked);
  nspc_add_value(env->curr, stmt->sym, stmt->v);
  return check_conts(env, stmt_self(stmt), stmt->body);
}

ANN static m_bool cond_type(const Env env, const Exp e) {
  if(e->next)
    ERR_B(e->pos, _("conditional must be a single expression"))
  const Type t = e->type;
  if(isa(t, t_int) > 0)
    return GW_OK;
  if(isa(t, t_float) > 0) {
    e->cast_to = t_int;
    return GW_OK;
  }
  ERR_B(e->pos, _("conditional must be of type 'int'..."))
}
#define stmt_func_xxx(name, type, prolog, exp) describe_stmt_func(check, name, type, prolog, exp)
stmt_func_xxx(if, Stmt_If,, !(!check_exp(env, stmt->cond) ||
  check_flow(env, stmt->cond, "if") < 0   ||
  check_stmt(env, stmt->if_body) < 0 ||
  (stmt->else_body && check_stmt(env, stmt->else_body) < 0)) ? 1 : -1)
stmt_func_xxx(flow, Stmt_Flow,,
  !(!check_exp(env, stmt->cond) ||
    check_flow(env, stmt->cond, stmt_self(stmt)->stmt_type == ae_stmt_while ? "while" : "until") < 0 ||
    check_conts(env, stmt_self(stmt), stmt->body) < 0) ? 1 : -1)
stmt_func_xxx(for, Stmt_For,, !(
  for_empty(env, stmt) < 0 ||
  check_stmt(env, stmt->c1) < 0 ||
  check_stmt(env, stmt->c2) < 0 ||
  check_flow(env, stmt->c2->d.stmt_exp.val, "for") < 0 ||
  (stmt->c3 && !check_exp(env, stmt->c3)) ||
  check_conts(env, stmt_self(stmt), stmt->body) < 0) ? 1 : -1)
stmt_func_xxx(loop, Stmt_Loop,, !(!check_exp(env, stmt->cond) ||
  cond_type(env, stmt->cond) < 0 ||
  check_conts(env, stmt_self(stmt), stmt->body) < 0) ? 1 : -1)
stmt_func_xxx(auto, Stmt_Auto,, do_stmt_auto(env, stmt))

ANN static m_bool check_stmt_return(const Env env, const Stmt_Exp stmt) {
  if(!env->func)
    ERR_B(stmt_self(stmt)->pos, _("'return' statement found outside function definition"))
  DECL_OB(const Type, ret_type, = stmt->val ? check_exp(env, stmt->val) : t_void)
  if(!env->func->def->base->ret_type) {
    assert(isa(env->func->value_ref->type, t_lambda) > 0);
    env->func->def->base->ret_type = ret_type;
    return GW_OK;
  }
  const struct Implicit imp = { stmt->val, env->func->def->base->ret_type, stmt_self(stmt)->pos };
  struct Op_Import opi = { .op=insert_symbol("@implicit"), .lhs=ret_type, .rhs=env->func->def->base->ret_type,
                      .data=(m_uint)&imp, .pos=stmt_self(stmt)->pos };
  const Type ret = op_check(env, &opi);
  if(!ret && isa(ret_type, env->func->def->base->ret_type) < 0)
    ERR_B(stmt_self(stmt)->pos, _("invalid return type '%s' -- expecting '%s'"),
          ret_type->name, env->func->def->base->ret_type->name)
  return GW_OK;
}

#define describe_check_stmt_stack(stack, name)                                     \
ANN static m_bool check_stmt_##name(const Env env, const Stmt stmt) {\
  if(!vector_size(&env->scope->stack))                                                    \
    ERR_B(stmt->pos, _("'"#name"' found outside of for/while/until..."))             \
  return GW_OK;                                                                        \
}
describe_check_stmt_stack(conts,  continue)
describe_check_stmt_stack(breaks, break)

ANN static m_bool check_stmt_jump(const Env env, const Stmt_Jump stmt) {
  if(stmt->is_label)
    return GW_OK;
  const Map label = env_label(env);
  const m_uint* key = env->class_def && !env->func ?
                (m_uint*)env->class_def : (m_uint*)env->func;
  const Map m = label->ptr ? (Map)map_get(label, (vtype)key) : NULL;
  if(!m)
    ERR_B(stmt_self(stmt)->pos, _("label '%s' used but not defined"), s_name(stmt->name))
  const Stmt_Jump ref = (Stmt_Jump)map_get(m, (vtype)stmt->name);
  if(!ref)
    ERR_B(stmt_self(stmt)->pos, _("label '%s' used but not defined"), s_name(stmt->name))
  vector_add(&ref->data.v, (vtype)stmt);
  return GW_OK;
}

ANN m_bool check_union_def(const Env env, const Union_Def udef) {
  if(tmpl_base(udef->tmpl)) // there's a func for this
    return GW_OK;
  if(udef->xid) {
    if(env->class_def)
      (!GET_FLAG(udef, static) ? decl_member : decl_static)(env, udef->value);
  } else if(env->class_def)  {
    if(!GET_FLAG(udef, static))
      udef->o = env->class_def->nspc->info->offset;
    else {
      udef->o = env->class_def->nspc->info->class_data_size;
      env->class_def->nspc->info->class_data_size += SZ_INT;
    }
  }
  const m_uint scope = union_push(env, udef);
  Decl_List l = udef->l;
  do {
    CHECK_OB(check_exp(env, l->self))
    if(isa(l->self->type, t_object) > 0) {
        if(!GET_FLAG(l->self->d.exp_decl.td, ref) && !GET_FLAG(udef->type, template))
      ERR_B(l->self->pos, _("In union, Objects must be declared as reference (use '@')"))
//      SET_FLAG(l->self->d.exp_decl.td, ref);
      Var_Decl_List list = l->self->d.exp_decl.list;
      do SET_FLAG(list->self->value, pure);
      while((list = list->next));
    }
    if(l->self->type->size > udef->s)
      udef->s = l->self->type->size;
  } while((l = l->next));
  union_pop(env, udef, scope);
  return GW_OK;
}

ANN static m_bool check_stmt_exp(const Env env, const Stmt_Exp stmt) {
  return stmt->val ? check_exp(env, stmt->val) ? 1 : -1 : 1;
}

ANN static Value match_value(const Env env, const Exp_Primary* prim, const m_uint i) {
  const Symbol sym = prim->d.var;
  const Value v = new_value(env->gwion->mp,
     ((Exp)VKEY(&env->scope->match->map, i))->type, s_name(sym));
  SET_FLAG(v, checked);
  nspc_add_value(env->curr, sym, v);
  VVAL(&env->scope->match->map, i) = (vtype)v;
  return v;
}

ANN static Symbol case_op(const Env env, const Exp e, const m_uint i) {
  if(e->exp_type == ae_exp_primary) {
    if(e->d.exp_primary.primary_type == ae_primary_unpack)
      return insert_symbol("@=>");
    else if(e->d.exp_primary.primary_type == ae_primary_id) {
      if(e->d.exp_primary.d.var == insert_symbol("_"))
        return NULL;
      if(!nspc_lookup_value1(env->curr, e->d.exp_primary.d.var)) {
        e->d.exp_primary.value = match_value(env, &e->d.exp_primary, i);
        return NULL;
      }
    }
  }
  return insert_symbol("==");
}

ANN static m_bool match_case_exp(const Env env, Exp e) {
  for(m_uint i = 0; i < map_size(&env->scope->match->map); e = e->next, ++i) {
    const Symbol op = case_op(env, e, i);
    if(op) {
      const Exp base = (Exp)VKEY(&env->scope->match->map, i);
      CHECK_OB(check_exp(env, e))
      Exp_Binary bin = { .lhs=base, .rhs=e, .op=op };
      struct Op_Import opi = { .op=op, .lhs=base->type, .rhs=e->type, .data=(uintptr_t)&bin, .pos=e->pos };
      CHECK_OB(op_check(env, &opi))
    }
  }
  return GW_OK;
}

ANN static m_bool _check_stmt_case(const Env env, const Stmt_Match stmt) {
  CHECK_BB(match_case_exp(env, stmt->cond))
  if(stmt->when)
    CHECK_OB(check_exp(env, stmt->when))
  return check_stmt_list(env, stmt->list);
}

ANN static m_bool check_stmt_case(const Env env, const Stmt_Match stmt) {
  RET_NSPC(_check_stmt_case(env, stmt))
}

ANN static m_bool case_loop(const Env env, const Stmt_Match stmt) {
  Stmt_List list = stmt->list;
  do CHECK_BB(check_stmt_case(env, &list->stmt->d.stmt_match))
  while((list = list->next));
  return GW_OK;
}

ANN static m_bool check_match(const Env env, const Stmt_Match stmt) {
  CHECK_OB(check_exp(env, stmt->cond))
  return case_loop(env, stmt);
}

ANN static m_bool _check_stmt_match(const Env env, const Stmt_Match stmt) {
  if(stmt->where)
    CHECK_BB(check_stmt(env, stmt->where))
  MATCH_INI(env->scope)
  const m_bool ret = check_match(env, stmt);
  MATCH_END(env->scope)
  return ret;
}

ANN static m_bool check_stmt_match(const Env env, const Stmt_Match stmt) {
  RET_NSPC(_check_stmt_match(env, stmt))
}

static const _exp_func stmt_func[] = {
  (_exp_func)check_stmt_exp,   (_exp_func)check_stmt_flow,     (_exp_func)check_stmt_flow,
  (_exp_func)check_stmt_for,   (_exp_func)check_stmt_auto,     (_exp_func)check_stmt_loop,
  (_exp_func)check_stmt_if,    (_exp_func)check_stmt_code,
  (_exp_func)check_stmt_break, (_exp_func)check_stmt_continue, (_exp_func)check_stmt_return,
  (_exp_func)check_stmt_match, (_exp_func)check_stmt_case,
  (_exp_func)check_stmt_jump,
};

ANN m_bool check_stmt(const Env env, const Stmt stmt) {
  return stmt_func[stmt->stmt_type](env, &stmt->d);
}

ANN static m_bool check_stmt_list(const Env env, Stmt_List l) {
  do CHECK_BB(check_stmt(env, l->stmt))
  while((l = l->next));
  return GW_OK;
}

ANN static m_bool check_signature_match(const Env env, const Func_Def fdef, const Func parent) {
  if(GET_FLAG(parent->def, static) != GET_FLAG(fdef, static)) {
    const m_str c_name  = fdef->base->func->value_ref->owner_class->name;
    const m_str p_name = parent->value_ref->owner_class->name;
    const m_str f_name = s_name(fdef->base->xid);
    ERR_B(td_pos(fdef->base->td),
          _("function '%s.%s' ressembles '%s.%s' but cannot override...\n"
          "  ...(reason: '%s.%s' is declared as 'static')"),
          c_name, f_name, p_name, c_name,
          GET_FLAG(fdef, static) ? c_name : p_name, f_name)
  }
  return !fdef->base->tmpl ? isa(fdef->base->ret_type, parent->def->base->ret_type) : GW_OK;
}

ANN static m_bool parent_match_actual(const Env env, const restrict Func_Def fdef,
    const restrict Func func) {
  Func parent_func = func;
  do {
    if(compat_func(fdef, parent_func->def) > 0) {
      CHECK_BB(check_signature_match(env, fdef, parent_func))
      if(!fdef->base->tmpl) {
        fdef->base->func->vt_index = parent_func->vt_index;
        vector_set(&env->curr->info->vtable, fdef->base->func->vt_index, (vtype)fdef->base->func);
      }
      return GW_OK;
    }
  } while((parent_func = parent_func->next));
  return 0;
}

ANN static m_bool check_parent_match(const Env env, const Func_Def fdef) {
  const Func func = fdef->base->func;
  const Type parent = env->class_def->e->parent;
  if(!env->curr->info->vtable.ptr)
    vector_init(&env->curr->info->vtable);
  if(parent) {
    const Value v = find_value(parent, fdef->base->xid);
    if(v && isa(v->type, t_function) > 0) {
      const m_bool match = parent_match_actual(env, fdef, v->d.func_ref);
      if(match)
        return match;
    }
  }
  func->vt_index = vector_size(&env->curr->info->vtable);
  vector_add(&env->curr->info->vtable, (vtype)func);
  return GW_OK;
}

ANN static inline Func get_overload(const Env env, const Func_Def fdef, const m_uint i) {
  const Symbol sym = func_symbol(env, env->curr->name, s_name(fdef->base->xid), NULL, i);
  return nspc_lookup_func1(env->curr, sym);
}

ANN static m_bool check_func_overload(const Env env, const Func_Def fdef) {
  const Value v = fdef->base->func->value_ref;
  for(m_uint i = 0; i <= v->offset; ++i) {
    const Func f1 = get_overload(env, fdef, i);
    for(m_uint j = i + 1; f1 && j <= v->offset; ++j) {
      const Func f2 = get_overload(env, fdef, j);
      if(f2 && compat_func(f1->def, f2->def) > 0)
        ERR_B(td_pos(f2->def->base->td), _("global function '%s' already defined"
          " for those arguments"), s_name(fdef->base->xid))
    }
  }
  return GW_OK;
}

ANN static m_bool check_func_def_override(const Env env, const Func_Def fdef) {
  const Func func = fdef->base->func;
  if(env->class_def && env->class_def->e->parent) {
    const Value override = find_value(env->class_def->e->parent, fdef->base->xid);
    if(override && override->owner_class && isa(override->type, t_function) < 0)
      ERR_B(fdef->pos,
            _("function name '%s' conflicts with previously defined value...\n"
            "  from super class '%s'..."),
            s_name(fdef->base->xid), override->owner_class->name)
  }
  if(func->value_ref->offset && (!fdef->base->tmpl || !fdef->base->tmpl->base))
    CHECK_BB(check_func_overload(env, fdef))
  return GW_OK;
}

ANN static Value set_variadic(const Env env) {
  const Value variadic = new_value(env->gwion->mp, t_vararg, "vararg");
  SET_FLAG(variadic, checked);
  nspc_add_value(env->curr, insert_symbol("vararg"), variadic);
  return variadic;
}

ANN static void operator_func(const Func f) {
  const Arg_List a = f->def->base->args;
  const m_bool is_unary = GET_FLAG(f->def, unary);
  const Type l = is_unary ? NULL : a->type;
  const Type r = is_unary ? a->type : a->next ? a->next->type : NULL;
  struct Op_Import opi = { .op=f->def->base->xid, .lhs=l, .rhs=r, .data=(m_uint)f, .pos=f->def->pos };
  operator_set_func(&opi);
}

ANN m_bool check_func_def(const Env env, const Func_Def fdef) {
  const Func func = get_func(env, fdef);
  m_bool ret = GW_OK;
  if(tmpl_base(fdef->base->tmpl))
    return env->class_def ? check_parent_match(env, fdef) : 1;
  if(fdef->base->td && !fdef->base->td->xid) { // tmpl ?
    fdef->base->ret_type = check_td(env, fdef->base->td);
    return traverse_func_def(env, fdef);
  }
  CHECK_BB(check_func_def_override(env, fdef))
  DECL_BB(const m_int, scope, = GET_FLAG(fdef, global) ? env_push_global(env) : env->scope->depth)
  if(env->class_def) // tmpl ?
    CHECK_BB(check_parent_match(env, fdef))
  const Func former = env->func;
  env->func = func;
  ++env->scope->depth;
  nspc_push_value(env->gwion->mp, env->curr);
  if(fdef->base->tmpl)
    CHECK_BB(template_push_types(env, fdef->base->tmpl))
  if(!fdef->base->args)
    UNSET_FLAG(fdef->base->func, pure);
  else
    ret = check_func_args(env, fdef->base->args);
  if(ret > 0) {
    const Value variadic = GET_FLAG(fdef, variadic) ? set_variadic(env) : NULL;
    if(!GET_FLAG(fdef, builtin) && fdef->d.code &&
        check_stmt_code(env, &fdef->d.code->d.stmt_code) < 0)
      ret = GW_ERROR;
    if(variadic)
      REM_REF(variadic, env->gwion)
    if(GET_FLAG(fdef, builtin))
      func->code->stack_depth = fdef->stack_depth;
    else if(GET_FLAG(fdef, op))
      operator_func(func);
  }
  if(fdef->base->tmpl)
    nspc_pop_type(env->gwion->mp, env->curr);
  nspc_pop_value(env->gwion->mp, env->curr);
  --env->scope->depth;
  env->func = former;
  if(GET_FLAG(fdef, global))
    env_pop(env,scope);
  return ret;
}

#define check_fptr_def dummy_func
DECL_SECTION_FUNC(check)

ANN static m_bool check_class_parent(const Env env, const Class_Def cdef) {
  const Type parent = cdef->base.type->e->parent;
  const Type_Decl *td = cdef->base.ext;
  if(td->array)
    CHECK_BB(check_exp_array_subscripts(env, td->array->exp))
  if(parent->e->def && !GET_FLAG(parent, check))
    CHECK_BB(scanx_parent(parent, traverse_cdef, env))
  if(GET_FLAG(parent, typedef))
    SET_FLAG(cdef->base.type, typedef);
  return GW_OK;
}

ANN /*static inline */void inherit(const Type t) {
  const Nspc nspc = t->nspc, parent = t->e->parent->nspc;
  nspc->info->offset = parent->info->offset;
  if(parent->info->vtable.ptr)
    vector_copy2(&parent->info->vtable, &nspc->info->vtable);
}

ANN m_bool check_class_def(const Env env, const Class_Def cdef) {
  if(tmpl_base(cdef->base.tmpl))
    return GW_OK;
  const Type type = cdef->base.type;
  SET_FLAG(type, check);
  if(type->e->parent == t_undefined) {
    type->e->parent = check_td(env, cdef->base.ext);
    return traverse_cdef(env, cdef);
  }
  if(cdef->base.ext)
    CHECK_BB(scanx_parent(cdef->base.type, check_class_parent, env))
  else if(!type->e->parent)
    type->e->parent = t_object;
  inherit(type);
  if(cdef->body)
    CHECK_BB(env_body(env, cdef, check_section))
  SET_FLAG(type, checked);
  return GW_OK;
}

ANN m_bool check_ast(const Env env, Ast ast) {
  do CHECK_BB(check_section(env, ast->section))
  while((ast = ast->next));
  return GW_OK;
}
