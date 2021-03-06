#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "instr.h"
#include "object.h"
#include "gwion.h"
#include "operator.h"
#include "import.h"
#include "vararg.h"
#include "gwi.h"

static MFUN(m_test) {
  printf("%p\n", *(M_Object*)MEM(0));
}

static MFUN(m_variadic) {
  M_Object str_obj = *(M_Object*)MEM(SZ_INT);
  if(!str_obj)return;
  m_str str = STRING(str_obj);
  const M_Object vararg_obj = *(M_Object*)MEM(SZ_INT*2);
  struct Vararg_* arg = *(struct Vararg_**)vararg_obj->data;

  m_uint i = 0;
  while(i < arg->s) {
    if(*str == 'i') {
      printf("%" INT_F "\n", *(m_int*)(arg->d + arg->o));
      arg->o += SZ_INT;
    } else if(*str == 'f') {
      printf("%f\n", *(m_float*)(arg->d + arg->o));
      arg->o += SZ_FLOAT;
    } else if(*str == 'o') {
      printf("%p\n", (void*)*(M_Object*)(arg->d + arg->o));
      arg->o += SZ_INT;
    }
    ++i;
    str++;
  }
}

GWION_IMPORT(variadic test) {
  GWI_OB(gwi_class_ini(gwi, "Variadic", NULL))
  GWI_BB(gwi_func_ini(gwi, "void", "member"))
  GWI_BB(gwi_func_arg(gwi, "string", "format"))
  GWI_BB(gwi_func_end(gwi, m_variadic, ae_flag_variadic))
  GWI_BB(gwi_func_ini(gwi, "void", "test"))
  GWI_BB(gwi_func_end(gwi, m_test, ae_flag_none))
  GWI_BB(gwi_class_end(gwi))
  return GW_OK;
}
