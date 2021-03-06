#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "object.h"
#include "instr.h"
#include "gwion.h"
#include "operator.h"
#include "import.h"

GWION_IMPORT(typedef_test) {
  GWI_BB(gwi_typedef_ini(gwi, "int", "<~A~>Typedef"))
  GWI_BB(gwi_typedef_ini(gwi, "int", "<~A~>Typedef"))
  return GW_OK;
}
