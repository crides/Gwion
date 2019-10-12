#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "type.h"
#include "object.h"
#include "instr.h"
#include "gwion.h"
#include "value.h"
#include "operator.h"
#include "import.h"
#include "gwi.h"

static m_int o_map_key;
static m_int o_map_value;
#define MAP_KEY(a) *((M_Object*)(a->data + o_map_key))
#define MAP_VAL(a) *((M_Object*)(a->data + o_map_value))
static CTOR(class_template_ctor) {
  /*char* name = strdup(o->type_ref->name);*/
  /*char* tmp = strsep(&name, "@");*/
  /*char* name1 = strsep(&name, "@");*/
/*Type t1 = nspc_lookup_type1(o->type_ref->info->parent, insert_symbol(name1));*/
  /*Type t2 = nspc_lookup_type0(shred->vm->emit->env->curr, insert_symbol(name));*/
/*free(tmp);*/
/**(M_Object*)(o->data) = new_array(t1->size, 0, t1->array_depth);*/
  /**(M_Object*)(o->data + SZ_INT) = new_array(t2->size, 0, t2->array_depth);*/
}

GWION_IMPORT(class_template) {
  Type t_class_template;
  const m_str list[2] = { "A", "B" };
  gwi_tmpl_ini(gwi, 2, list);
  GWI_OB((t_class_template = gwi_mk_type(gwi, "ClassTemplate", SZ_INT, "Object")))
  GWI_BB(gwi_class_ini(gwi, t_class_template, class_template_ctor, NULL))
  gwi_tmpl_end(gwi);
  GWI_BB(gwi_item_ini(gwi, "A[]", "key"))
    GWI_BB((o_map_key = gwi_item_end(gwi, ae_flag_member | ae_flag_template, NULL)))
    GWI_BB(gwi_item_ini(gwi, "B[]", "value"))
    GWI_BB((o_map_value = gwi_item_end(gwi, ae_flag_member, NULL)))
  GWI_BB(gwi_class_end(gwi))

  GWI_BB(gwi_item_ini(gwi, "ClassTemplate<~Ptr<~int~>,int~>", "testObject"))
  GWI_BB(gwi_item_end(gwi, ae_flag_none, NULL))
  return GW_OK;
}
