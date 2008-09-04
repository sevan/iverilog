/*
 * Copyright (c) 2001-2008 Stephen Williams (steve@icarus.com)
 *
 *    This source code is free software; you can redistribute it
 *    and/or modify it in source code form under the terms of the GNU
 *    General Public License as published by the Free Software
 *    Foundation; either version 2 of the License, or (at your option)
 *    any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

# include  "compile.h"
# include  "vpi_priv.h"
# include  "symbols.h"
# include  "statistics.h"
#ifdef HAVE_MALLOC_H
# include  <malloc.h>
#endif
# include  <string.h>
# include  <stdlib.h>
# include  <assert.h>

static vpiHandle *vpip_root_table_ptr = 0;
static unsigned   vpip_root_table_cnt = 0;

vpiHandle vpip_make_root_iterator(void)
{
      assert(vpip_root_table_ptr);
      assert(vpip_root_table_cnt);
      return vpip_make_iterator(vpip_root_table_cnt,
				vpip_root_table_ptr, false);
}

void vpip_make_root_iterator(struct __vpiHandle**&table, unsigned&ntable)
{
      table = vpip_root_table_ptr;
      ntable = vpip_root_table_cnt;
}

static bool handle_is_scope(vpiHandle obj)
{
      return (obj->vpi_type->type_code == vpiModule)
	    || (obj->vpi_type->type_code == vpiFunction)
	    || (obj->vpi_type->type_code == vpiTask)
	    || (obj->vpi_type->type_code == vpiNamedBegin)
	    || (obj->vpi_type->type_code == vpiNamedFork);
}

static int scope_get(int code, vpiHandle obj)
{
      struct __vpiScope*ref = (struct __vpiScope*)obj;

      assert(handle_is_scope(obj));

      switch (code) {
	  case vpiDefLineNo:
	    return ref->def_lineno;

	  case vpiLineNo:
	    return ref->lineno;

	  case vpiTimeUnit:
	    return ref->time_units;

	  case vpiTimePrecision:
	    return ref->time_precision;

	  case vpiTopModule:
	    return 0x0 == ref->scope;
      }

      return 0;
}

static void construct_scope_fullname(struct __vpiScope*ref, char*buf)
{
      if (ref->scope) {
	    construct_scope_fullname(ref->scope, buf);
	    strcat(buf, ".");
      }

      strcat(buf, ref->name);
}

static const char* scope_get_type(int code)
{
      switch (code) {
          case vpiModule:
            return "vpiModule";
          case vpiFunction:
            return "vpiFunction";
          case vpiTask:
            return "vpiTask";
          case vpiNamedBegin:
            return "vpiNamedBegin";
          case vpiNamedFork:
            return "vpiNamedFork";
          default:
            fprintf(stderr, "ERROR: invalid code %d.", code);
            assert(0);
      }
}

static char* scope_get_str(int code, vpiHandle obj)
{
      struct __vpiScope*ref = (struct __vpiScope*)obj;

      assert(handle_is_scope(obj));

      char buf[4096];  // XXX is a fixed buffer size really reliable?
      const char *p=0;
      switch (code) {
	  case vpiDefFile:
	    p = file_names[ref->def_file_idx];
	    break;

	  case vpiFile:
	    p = file_names[ref->file_idx];
	    break;

	  case vpiFullName:
	    buf[0] = 0;
	    construct_scope_fullname(ref, buf);
	    p = buf;
	    break;

	  case vpiName:
	    p = ref->name;
	    break;

	  case vpiDefName:
	    p = ref->tname;
	    break;

	  case vpiType:
	    p = scope_get_type(code);
	    break;

	  default:
	    fprintf(stderr, "ERROR: invalid code %d.", code);
	    assert(0);
	    return 0;
      }
      return simple_set_rbuf_str(p);
}

static vpiHandle scope_get_handle(int code, vpiHandle obj)
{
      assert((obj->vpi_type->type_code == vpiModule)
	     || (obj->vpi_type->type_code == vpiFunction)
	     || (obj->vpi_type->type_code == vpiTask)
	     || (obj->vpi_type->type_code == vpiNamedBegin)
	     || (obj->vpi_type->type_code == vpiNamedFork));

      struct __vpiScope*rfp = (struct __vpiScope*)obj;

      switch (code) {

	  case vpiScope:
	    return &rfp->scope->base;

	  case vpiModule:
	    return &rfp->scope->base;
      }

      return 0;
}

/* compares vpiType's considering object classes */
static int compare_types(int code, int type)
{
	/* NOTE: The Verilog VPI does not for any object support
	   vpiScope as an iterator parameter, so it is used here as a
	   means to scan everything in the *current* scope. */
      if (code == vpiScope)
	    return 1;

      if (code == type)
	    return 1;

      if ( code == vpiInternalScope &&
	     (type == vpiModule ||
	     type == vpiFunction ||
	     type == vpiTask ||
	     type == vpiNamedBegin ||
	     type == vpiNamedFork) )
	    return 1;

      if ( code == vpiVariables &&
	     (type == vpiIntegerVar ||
	      type == vpiTimeVar    ||
	      type == vpiRealVar))
	    return 1;

      return 0;
}

static vpiHandle module_iter_subset(int code, struct __vpiScope*ref)
{
      unsigned mcnt = 0, ncnt = 0;
      vpiHandle*args;

      for (unsigned idx = 0 ;  idx < ref->nintern ;  idx += 1)
	    if (compare_types(code, ref->intern[idx]->vpi_type->type_code))
		  mcnt += 1;

      if (mcnt == 0)
	    return 0;

      args = (vpiHandle*)calloc(mcnt, sizeof(vpiHandle));
      for (unsigned idx = 0 ;  idx < ref->nintern ;  idx += 1)
	    if (compare_types(code, ref->intern[idx]->vpi_type->type_code))
		  args[ncnt++] = ref->intern[idx];

      assert(ncnt == mcnt);

      return vpip_make_iterator(mcnt, args, true);
}

/*
 * This function implements the vpi_iterate method for vpiModule and
 * similar objects. The vpi_iterate allows the user to iterate over
 * things that are contained in the scope object, by generating an
 * iterator for the requested set of items.
 */
static vpiHandle module_iter(int code, vpiHandle obj)
{
      struct __vpiScope*ref = (struct __vpiScope*)obj;
      assert((obj->vpi_type->type_code == vpiModule)
	     || (obj->vpi_type->type_code == vpiFunction)
	     || (obj->vpi_type->type_code == vpiTask)
	     || (obj->vpi_type->type_code == vpiNamedBegin)
	     || (obj->vpi_type->type_code == vpiNamedFork));

      return module_iter_subset(code, ref);
}


static const struct __vpirt vpip_scope_module_rt = {
      vpiModule,
      scope_get,
      scope_get_str,
      0,
      0,
      scope_get_handle,
      module_iter
};

static const struct __vpirt vpip_scope_task_rt = {
      vpiTask,
      scope_get,
      scope_get_str,
      0,
      0,
      scope_get_handle,
      module_iter
};

static const struct __vpirt vpip_scope_function_rt = {
      vpiFunction,
      scope_get,
      scope_get_str,
      0,
      0,
      scope_get_handle,
      module_iter
};

static const struct __vpirt vpip_scope_begin_rt = {
      vpiNamedBegin,
      scope_get,
      scope_get_str,
      0,
      0,
      scope_get_handle,
      module_iter
};

static const struct __vpirt vpip_scope_fork_rt = {
      vpiNamedFork,
      scope_get,
      scope_get_str,
      0,
      0,
      scope_get_handle,
      module_iter
};

/*
 * The current_scope is a compile time concept. As the vvp source is
 * compiled, items that have scope are placed in the current
 * scope. The ".scope" directives select the scope that is current.
 */
static struct __vpiScope*current_scope = 0;

static void attach_to_scope_(struct __vpiScope*scope, vpiHandle obj)
{
      assert(scope);
      unsigned idx = scope->nintern++;

      if (scope->intern == 0)
	    scope->intern = (vpiHandle*)
		  malloc(sizeof(vpiHandle));
      else
	    scope->intern = (vpiHandle*)
		  realloc(scope->intern, sizeof(vpiHandle)*scope->nintern);

      scope->intern[idx] = obj;
}

/*
 * When the compiler encounters a scope declaration, this function
 * creates and initializes a __vpiScope object with the requested name
 * and within the addressed parent. The label is used as a key in the
 * symbol table and the name is used to construct the actual object.
 */
void
compile_scope_decl(char*label, char*type, char*name, const char*tname,
                   char*parent, long file_idx, long lineno,
                   long def_file_idx, long def_lineno)
{
      struct __vpiScope*scope = new struct __vpiScope;
      count_vpi_scopes += 1;

      if (strcmp(type,"module") == 0)
	    scope->base.vpi_type = &vpip_scope_module_rt;
      else if (strcmp(type,"function") == 0)
	    scope->base.vpi_type = &vpip_scope_function_rt;
      else if (strcmp(type,"task") == 0)
	    scope->base.vpi_type = &vpip_scope_task_rt;
      else if (strcmp(type,"fork") == 0)
	    scope->base.vpi_type = &vpip_scope_fork_rt;
      else if (strcmp(type,"begin") == 0)
	    scope->base.vpi_type = &vpip_scope_begin_rt;
      else if (strcmp(type,"generate") == 0)
	    scope->base.vpi_type = &vpip_scope_begin_rt;
      else {
	    scope->base.vpi_type = &vpip_scope_module_rt;
	    assert(0);
      }

      assert(scope->base.vpi_type);

      scope->name = vpip_name_string(name);
      scope->tname = vpip_name_string(tname);
      scope->file_idx = (unsigned) file_idx;
      scope->lineno  = (unsigned) lineno;
      scope->def_file_idx = (unsigned) def_file_idx;
      scope->def_lineno  = (unsigned) def_lineno;
      scope->intern = 0;
      scope->nintern = 0;
      scope->threads = 0;

      current_scope = scope;

      compile_vpi_symbol(label, &scope->base);

      free(label);
      free(type);
      free(name);

      if (parent) {
	    static vpiHandle obj;
	    compile_vpi_lookup(&obj, parent);
	    assert(obj);
	    struct __vpiScope*sp = (struct __vpiScope*) obj;
	    attach_to_scope_(sp, &scope->base);
	    scope->scope = (struct __vpiScope*)obj;

	      /* Inherit time units and precision from the parent scope. */
	    scope->time_units = sp->time_units;
	    scope->time_precision = sp->time_precision;

      } else {
	    scope->scope = 0x0;

	    unsigned cnt = vpip_root_table_cnt + 1;
	    vpip_root_table_ptr = (vpiHandle*)
		  realloc(vpip_root_table_ptr, cnt * sizeof(vpiHandle));
	    vpip_root_table_ptr[vpip_root_table_cnt] = &scope->base;
	    vpip_root_table_cnt = cnt;

	      /* Root scopes inherit time_units and precision from the
	         system precision. */
	    scope->time_units = vpip_get_time_precision();
	    scope->time_precision = vpip_get_time_precision();
      }
}

void compile_scope_recall(char*symbol)
{
      compile_vpi_lookup((vpiHandle*)&current_scope, symbol);
      assert(current_scope);
}

/*
 * This function handles the ".timescale" directive in the vvp
 * source. It sets in the current scope the specified units value.
 */
void compile_timescale(long units, long precision)
{
      assert(current_scope);
      current_scope->time_units = units;
      current_scope->time_precision = precision;
}

struct __vpiScope* vpip_peek_current_scope(void)
{
      return current_scope;
}

void vpip_attach_to_current_scope(vpiHandle obj)
{
      attach_to_scope_(current_scope, obj);
}
