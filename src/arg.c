#include <string.h>
#include "gwion_util.h"
#include "arg.h"
#include "soundinfo.h"
#define GWIONRC ".gwionrc"

/* use before MemPool allocation */
ANN static inline void config_end(const Vector config) {
  for(m_uint i = 0; i < vector_size(config); ++i) {
    const Vector v = (Vector)vector_at(config, i);
    for(m_uint i = 1; i < vector_size(v); ++i)
      xfree((m_str)vector_at(v, i));
    vector_release(v);
    xfree(v);
  }
}

ANN static void arg_init(Arg* arg) {
  vector_init(&arg->add);
  vector_init(&arg->lib);
  vector_init(&arg->mod);
  vector_init(&arg->config);
  vector_add(&arg->lib, (vtype)GWPLUG_DIR);
}

ANN void arg_release(Arg* arg) {
  vector_release(&arg->add);
  vector_release(&arg->lib);
  vector_release(&arg->mod);
  config_end(&arg->config);
  vector_release(&arg->config);
}

static const char usage[] =
"usage: Gwion <options>\n"
"\t-h\t             : this help\n"
"\t-k\t             : show compilation flags\n"
"\t-c\t  <file>     : load config\n"
"\t-s\t  <number>   : set samplerate\n"
"\t-i\t  <number>   : set input channel number\n"
"\t-o\t  <number>   : set output channel number\n"
"\t-d\t  <number>   : set driver (and arguments)\n"
"\t-m\t  <number>   : load module (and arguments)\n"
"\t-p\t <directory> : add a plugin directory\n";

ANN static void config_parse(Arg* arg, const m_str name);

#define CASE(a,b) case a: (b) ; break;
#define get_arg(a) (a[i][2] == '\0' ? arg->argv[++i] : a[i] + 2)
#define ARG2INT(a) strtol(get_arg(a), NULL, 10)

ANN void _arg_parse(Arg* arg) {
  for(int i = 1; i < arg->argc; ++i) {
    if(arg->argv[i][0] == '-') {
      switch(arg->argv[i][1]) {
        CASE('h', gw_err(usage))
        CASE('k', gw_err("CFLAGS: %s\nLDFLAGS: %s\n", CFLAGS, LDFLAGS))
        CASE('c', config_parse(arg, get_arg(arg->argv)))
        CASE('p', vector_add(&arg->lib, (vtype)get_arg(arg->argv)))
        CASE('m', vector_add(&arg->mod, (vtype)get_arg(arg->argv)))
        CASE('l', arg->loop = (m_bool)ARG2INT(arg->argv) > 0 ? 1 : -1)
        CASE('i', arg->si->in  = (uint8_t)ARG2INT(arg->argv))
        CASE('o', arg->si->out = (uint8_t)ARG2INT(arg->argv))
        CASE('s', arg->si->sr = (uint32_t)ARG2INT(arg->argv))
        CASE('d', arg->si->arg = get_arg(arg->argv))
      }
    } else
      vector_add(&arg->add, (vtype)arg->argv[i]);
  }
}

ANN static void split_line(const m_str line, const Vector v) {
  m_str d = strdup(line), c = d;
  while(d) {
    const m_str str = strsep(&d, " ");
    const size_t sz = strlen(str);
    const m_bool arg = (str[sz-1] == '\n');
    vector_add(v, (vtype)strndup(str, arg ? sz - 1 : sz));
  }
  free(d);
  free(c);
}

ANN static Vector get_config(const m_str name) {
  char *line = NULL;
  size_t len = 0;
  ssize_t nread;
  FILE *f = fopen(name, "r");
  CHECK_OO(f)
  const Vector v = (Vector)xmalloc(sizeof(struct Vector_));
  vector_init(v);
  vector_add(v, (vtype)name);
  while((nread = getline(&line, &len, f)) != -1) {
    if(line[0] != '#')
      split_line(line, v);
  }
  free(line);
  fclose(f);
  return v;
}

ANN static void config_parse(Arg* arg, const m_str name) {
  const Vector v = get_config(name);
  if(v) {
    int argc = arg->argc;
    char** argv = arg->argv;
    arg->argc = vector_size(v);
    arg->argv =  (m_str*)(v->ptr + OFFSET);
    _arg_parse(arg);
    arg->argc = argc;
    arg->argv = argv;
    vector_add(&arg->config, (vtype)v);
  }
}

ANN static void config_default(Arg* arg) {
  char* home = getenv("HOME");
  char c[strlen(home) + strlen(GWIONRC) + 2];
  sprintf(c, "%s/%s", home, GWIONRC);
  config_parse(arg, c);
}

ANN void arg_parse(Arg* a) {
  arg_init(a);
  config_default(a);
  _arg_parse(a);
}
