#include <debase_internals.h>
#include <inttypes.h> // defines uint32_t
#include <stddef.h> // defines size_t
#include <string.h> // for strncmp


// From https://stackoverflow.com/a/24753227
typedef uint32_t bitarray_t;
#define RESERVE_BITS(n) (((n)+0x1f)>>5)
#define DW_INDEX(x) ((x)>>5)
#define BIT_INDEX(x) ((x)&0x1f)
#define GETBIT(array,index) (((array)[DW_INDEX(index)]>>BIT_INDEX(index))&1)
#define PUTBIT(array, index, bit) \
    ((bit)&1 ?  ((array)[DW_INDEX(index)] |= 1<<BIT_INDEX(index)) \
             :  ((array)[DW_INDEX(index)] &= ~(1<<BIT_INDEX(index))) \
             , 0 \
    )
#define CLEARBITS(array, size) for (int i = 0; i < RESERVE_BITS(size); i++) { array[i] = 0; }
// Can unique identify lines 0 thru 65535 with breakpoints. Larger values wrap around.
#define BIT_ARRAY_SIZE 0xFFFF

#ifdef _WIN32
#include <ctype.h>
#endif

#if defined DOSISH
#define isdirsep(x) ((x) == '/' || (x) == '\\')
#else
#define isdirsep(x) ((x) == '/')
#endif

static int disableCache;
static VALUE cBreakpoint;
static int breakpoint_max;
// If bit x is set, then a breakpoint on line x (or x % BIT_ARRAY_SIZE) exists and may be active.
static bitarray_t existing_breakpoint_lines[RESERVE_BITS(BIT_ARRAY_SIZE)];

#define HASH_MULT 65599; // sdbm

inline unsigned int mix(unsigned int acc, unsigned int nw) {
    return nw + (acc << 6) + (acc << 16) - acc; // HASH_MULT in faster version
}

inline unsigned int hash(char *str, size_t len) {
    unsigned int res = 0;
    for (size_t i = 0; i < len; i++) {
        res = mix(res, str[i] - '!'); // "!" is the first printable letter in ASCII.
        // This will help Latin1 but may harm utf8 multibyte
    }
    return res * HASH_MULT;
}

#define REALPATH_CACHE_SIZE 2048

static int cache_miss_count;

typedef struct realpath_result_t {
  // Non-null-terminated string.
  char *str;
  size_t str_len;
} realpath_result_t;

#ifdef PATH_MAX
typedef struct cached_string_t {
  // String. May not be null terminated.
  char str[PATH_MAX + 1];
  // Length of string (not including null terminators).
  size_t str_len;
} cached_string_t;

typedef struct cache_entry_t {
  cached_string_t path;
  cached_string_t realpath;
} cache_entry_t;

cache_entry_t realpath_cache[REALPATH_CACHE_SIZE];
static realpath_result_t uncached_realpath(char *path, size_t len, cached_string_t *cache_realpath) {
  realpath_result_t rv;
  if (realpath(path, cache_realpath->str) != NULL) {
    cache_realpath->str_len = strnlen(cache_realpath->str, PATH_MAX);
  } else {
    // Realpath failed. Use `path` as the realpath.
    cache_realpath->str_len = len;
    strncpy(cache_realpath->str, path, len);
  }
  rv.str = cache_realpath->str;
  rv.str_len = cache_realpath->str_len;
  return rv;
}

static realpath_result_t realpath_cached(char* path, size_t len)
{
  if (disableCache) {
    return uncached_realpath(path, len, &realpath_cache[0].path);
  }
  const unsigned int index = hash(path, len) % REALPATH_CACHE_SIZE;
  cache_entry_t *entry = &realpath_cache[index]; 
  cached_string_t* entry_path = &entry->path;
  cached_string_t* entry_realpath = &entry->realpath;
  realpath_result_t rv;
  if (entry_path->str_len == len && strncmp(path, entry_path->str, len) == 0) {
    // Cache hit.
    rv.str = entry_realpath->str;
    rv.str_len = entry_realpath->str_len;
    return rv;
  }
  cache_miss_count++;

  rv = uncached_realpath(path, len, entry_realpath);
  strncpy(entry_path->str, path, len);
  entry_path->str_len = len;

  return rv;
}

static void clearCache()
{
  for (int i = 0; i < REALPATH_CACHE_SIZE; i++) {
    realpath_cache[i].path.str_len = 0;
    realpath_cache[i].realpath.str_len = 0;
  }
}
#else
typedef struct cache_entry_t {
  realpath_result_t path;
  realpath_result_t realpath;
} cache_entry_t;

static void freeCachedString(realpath_result_t *str) {
  if (str->str != NULL) {
    free(str->str);
    str->str = NULL;
    str->str_len = 0;
  }
}

static realpath_result_t uncached_realpath(char *path, size_t len, realpath_result_t* rv) {
  freeCachedString(rv);
  rv->str = realpath(path, NULL);
  if (rv->str != NULL) {
    rv->str_len = strlen(rv->str);
  } else {
    // Realpath failed. Use `path` as the realpath.
    rv->str_len = len;
    rv->str = path;
  }
  return *rv;
}



static void freeCacheEntry(cache_entry_t *entry) {
  freeCachedString(&entry->path);
  freeCachedString(&entry->realpath);
}

// Entries alternate between rawPath and realPath.
static cache_entry_t realpath_cache[REALPATH_CACHE_SIZE];
static realpath_result_t realpath_cached(char* path, size_t len)
{
  if (disableCache) {
    return uncached_realpath(path, len, &realpath_cache[0].realpath);
  }
  unsigned int index = hash(path, len) % REALPATH_CACHE_SIZE;
  cache_entry_t *entry = &realpath_cache[index];
  realpath_result_t* entry_path = &entry->path;
  realpath_result_t* entry_realpath = &entry->realpath;
  if (!disableCache && entry_path->str_len == len && entry_path->str != NULL && strncmp(path, entry_path->str, len) == 0) {
    // Cache hit.
    return *entry_realpath;
  }
  cache_miss_count++;

  freeCacheEntry(entry);
  realpath_result_t rv = uncached_realpath(path, len, entry_realpath);
  entry_path->str = malloc(sizeof(char) * len);
  entry_path->str_len = len;
  strncpy(entry_path->str, path, len);
  return rv;
}


static void clearCache()
{
  for (int i = 0; i < REALPATH_CACHE_SIZE; i++) {
    freeCacheEntry(&realpath_cache[i]);
  }
}
#endif


static ID idEval;

static VALUE
eval_expression(VALUE args)
{
  return rb_funcall2(rb_mKernel, idEval, 2, RARRAY_PTR(args));
}

extern VALUE
catchpoint_hit_count(VALUE catchpoints, VALUE exception, VALUE *exception_name) {
  VALUE ancestors;
  VALUE expn_class;
  VALUE aclass;
  VALUE mod_name;
  VALUE hit_count;
  int i;

  if (catchpoints == Qnil /*|| st_get_num_entries(RHASH_TBL(rdebug_catchpoints)) == 0)*/)
    return Qnil;
  expn_class = rb_obj_class(exception);
  ancestors = rb_mod_ancestors(expn_class);
  for(i = 0; i < RARRAY_LENINT(ancestors); i++)
  {
    aclass    = rb_ary_entry(ancestors, i);
    mod_name  = rb_mod_name(aclass);
    hit_count = rb_hash_aref(catchpoints, mod_name);
    if(hit_count != Qnil)
    {
      *exception_name = mod_name;
      return hit_count;
    }
  }
  return Qnil;
}

static void
Breakpoint_mark(breakpoint_t *breakpoint)
{
  rb_gc_mark(breakpoint->source);
  rb_gc_mark(breakpoint->expr);
}

static VALUE
Breakpoint_create(VALUE klass)
{
    breakpoint_t *breakpoint;

    breakpoint = ALLOC(breakpoint_t);
    return Data_Wrap_Struct(klass, Breakpoint_mark, xfree, breakpoint);
}

static VALUE
Breakpoint_initialize(VALUE self, VALUE source, VALUE pos, VALUE expr)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  breakpoint->id = ++breakpoint_max;
  breakpoint->source = StringValue(source);
  breakpoint->line = FIX2INT(pos);
  breakpoint->enabled = Qtrue;
  breakpoint->expr = NIL_P(expr) ? expr : StringValue(expr);

  return Qnil;
}

static VALUE
Breakpoint_cache_misses(VALUE self)
{
  return INT2FIX(cache_miss_count);
}

static VALUE
Breakpoint_activate(VALUE self, VALUE breakpoints, VALUE id_value)
{
  int i;
  int id;
  VALUE breakpoint_object;
  breakpoint_t *breakpoint;

  if (breakpoints == Qnil) return Qnil;

  id = FIX2INT(id_value);

  for (i = 0; i < RARRAY_LENINT(breakpoints); i++)
  {
    breakpoint_object = rb_ary_entry(breakpoints, i);
    Data_Get_Struct(breakpoint_object, breakpoint_t, breakpoint);
    if(breakpoint->id == id)
    {
      PUTBIT(existing_breakpoint_lines, breakpoint->line % BIT_ARRAY_SIZE, 1);
      return Qnil;
    }
  }
  return Qnil;
}

static VALUE
Breakpoint_remove(VALUE self, VALUE breakpoints, VALUE id_value)
{
  int i;
  int id;
  VALUE breakpoint_object;
  VALUE breakpoint_object_to_return;
  breakpoint_t *breakpoint;

  if (breakpoints == Qnil) return Qnil;

  id = FIX2INT(id_value);
  breakpoint_object_to_return = Qnil;

  // Rebuild line bitvector while traversing breakpoints array.
  CLEARBITS(existing_breakpoint_lines, BIT_ARRAY_SIZE)
  for (i = 0; i < RARRAY_LENINT(breakpoints); i++)
  {
    breakpoint_object = rb_ary_entry(breakpoints, i);
    Data_Get_Struct(breakpoint_object, breakpoint_t, breakpoint);
    if (breakpoint->id == id)
    {
      rb_ary_delete_at(breakpoints, i);
      breakpoint_object_to_return = breakpoint_object;
    } else {
      PUTBIT(existing_breakpoint_lines, breakpoint->line, 1);
    }
  }
  return breakpoint_object_to_return;
}

static VALUE
Breakpoint_disable_cache(VALUE self, VALUE newValue) {
  disableCache = newValue == Qtrue ? 1 : 0;

  // Clear cache
  clearCache();

  return disableCache ? Qtrue : Qfalse;
}

static VALUE
Breakpoint_id(VALUE self)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  return INT2FIX(breakpoint->id);
}

static VALUE
Breakpoint_source(VALUE self)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  return breakpoint->source;
}

static VALUE
Breakpoint_expr_get(VALUE self)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  return breakpoint->expr;
}

static VALUE
Breakpoint_expr_set(VALUE self, VALUE new_val)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  breakpoint->expr = new_val;
  return breakpoint->expr;
}

static VALUE
Breakpoint_enabled_set(VALUE self, VALUE new_val)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  breakpoint->enabled = new_val;
  return breakpoint->enabled;
}

static VALUE
Breakpoint_enabled_get(VALUE self)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  return breakpoint->enabled;
}

static VALUE
Breakpoint_pos(VALUE self)
{
  breakpoint_t *breakpoint;

  Data_Get_Struct(self, breakpoint_t, breakpoint);
  return INT2FIX(breakpoint->line);
}

int
filename_cmp_impl(VALUE source, char *file, long f_len)
{
  char *source_ptr, *file_ptr;
  long s_len, min_len;
  long s,f;
  int dirsep_flag = 0;

  s_len = RSTRING_LEN(source);
  min_len = s_len < f_len ? s_len : f_len;

  source_ptr = RSTRING_PTR(source);
  file_ptr   = file;

  for( s = s_len - 1, f = f_len - 1; s >= s_len - min_len && f >= f_len - min_len; s--, f-- )
  {
    if((source_ptr[s] == '.' || file_ptr[f] == '.') && dirsep_flag)
      return 1;
    if(isdirsep(source_ptr[s]) && isdirsep(file_ptr[f]))
      dirsep_flag = 1;
#ifdef DOSISH_DRIVE_LETTER
    else if (s == 0)
      return(toupper(source_ptr[s]) == toupper(file_ptr[f]));
#endif
    else if(source_ptr[s] != file_ptr[f])
      return 0;
  }
  return 1;
}

int
filename_cmp(VALUE source, char *file, long file_len)
{
#ifdef _WIN32
  return filename_cmp_impl(source, file, file_len);
#else
  realpath_result_t path = realpath_cached(file, file_len);
  return filename_cmp_impl(source, path.str, path.str_len);
#endif  
}

static int
check_breakpoint_by_pos(VALUE breakpoint_object, char *file, long file_len, int line)
{
    breakpoint_t *breakpoint;

    if(breakpoint_object == Qnil)
        return 0;
    Data_Get_Struct(breakpoint_object, breakpoint_t, breakpoint);
    if (Qtrue != breakpoint->enabled) return 0;
    if (breakpoint->line != line)
        return 0;
    if(filename_cmp(breakpoint->source, file, file_len))
        return 1;
    return 0;
}

static int
check_breakpoint_expr(VALUE breakpoint_object, VALUE trace_point)
{
  breakpoint_t *breakpoint;
  VALUE binding, args, result;
  int error;

  if(breakpoint_object == Qnil) return 0;
  Data_Get_Struct(breakpoint_object, breakpoint_t, breakpoint);
  if (Qtrue != breakpoint->enabled) return 0;
  if (NIL_P(breakpoint->expr)) return 1;

  if (NIL_P(trace_point)) {
    binding = rb_const_get(rb_cObject, rb_intern("TOPLEVEL_BINDING"));
  } else {
    binding = rb_tracearg_binding(rb_tracearg_from_tracepoint(trace_point));
  }

  args = rb_ary_new3(2, breakpoint->expr, binding);
  result = rb_protect(eval_expression, args, &error);
  return !error && RTEST(result);
}

static VALUE
Breakpoint_find(VALUE self, VALUE breakpoints, VALUE source, VALUE pos, VALUE trace_point)
{
  return breakpoint_find(breakpoints, source, pos, trace_point);
}

extern VALUE
breakpoint_find(VALUE breakpoints, VALUE source, VALUE pos, VALUE trace_point)
{
  VALUE breakpoint_object;
  char *file;
  int line;
  int i;
  long file_len;

  
  line = FIX2INT(pos);
  // Fast reject: We have no breakpoints for this line.
  if (GETBIT(existing_breakpoint_lines, line % BIT_ARRAY_SIZE) == 0) {
    return Qnil;
  }

  file = RSTRING_PTR(source);
  file_len = RSTRING_LEN(source);
  for(i = 0; i < RARRAY_LENINT(breakpoints); i++)
  {
    breakpoint_object = rb_ary_entry(breakpoints, i);
    if (check_breakpoint_by_pos(breakpoint_object, file, file_len, line) &&
      check_breakpoint_expr(breakpoint_object, trace_point))
    {
      return breakpoint_object;
    }
  }
  return Qnil;
}

extern void
breakpoint_init_variables()
{
  breakpoint_max = 0;
}

extern void
Init_breakpoint(VALUE mDebase)
{
  breakpoint_init_variables();
  cBreakpoint = rb_define_class_under(mDebase, "Breakpoint", rb_cObject);
  rb_define_singleton_method(cBreakpoint, "find", Breakpoint_find, 4);
  rb_define_singleton_method(cBreakpoint, "remove", Breakpoint_remove, 2);
  rb_define_singleton_method(cBreakpoint, "activate", Breakpoint_activate, 2);
  rb_define_singleton_method(cBreakpoint, "cache_misses", Breakpoint_cache_misses, 0);
  rb_define_singleton_method(cBreakpoint, "disable_cache", Breakpoint_disable_cache, 1);
  rb_define_method(cBreakpoint, "initialize", Breakpoint_initialize, 3);
  rb_define_method(cBreakpoint, "id", Breakpoint_id, 0);
  rb_define_method(cBreakpoint, "source", Breakpoint_source, 0);
  rb_define_method(cBreakpoint, "pos", Breakpoint_pos, 0);

  /* <For tests> */
  rb_define_method(cBreakpoint, "expr", Breakpoint_expr_get, 0);
  rb_define_method(cBreakpoint, "expr=", Breakpoint_expr_set, 1);
  rb_define_method(cBreakpoint, "enabled", Breakpoint_enabled_get, 0);
  rb_define_method(cBreakpoint, "enabled=", Breakpoint_enabled_set, 1);
  /* </For tests> */

  rb_define_alloc_func(cBreakpoint, Breakpoint_create);

  idEval = rb_intern("eval");
  disableCache = 0;
}
