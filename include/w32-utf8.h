// expecting all the types used in this files already specified (#include).
// mu_... are mingw-utf8 utils
// foo_U is the utf8 variant of foo (e.g. fopen) or FooA (CreateProcessA)

char **mu_get_utf8_prog_argv(void);
void mu_init_utf8_env(void);
void mu_export_utf8_env(void);


// the utf8 _U APIs are easily exposed, as #define {foo,FooA} foo_U,
// without prior #undef so that it stands out if we're re-defining it.
//
// One such case that would re-define is some mingw/ansi APIs, like fopen,
// and in those cases the name is typically already mapped globally, and
// then #undef(ined) right before the mingw/winansi wrapper implementation
// (so that it can call the underlaying API with that name - which we want
// to map to the _U API), so we shouldn't override the name globally.
//
// In those cases we do one of two things:
// - if the API foo is a "deprecated alias" to _foo then we map _foo to foo_U
//   and in the wrapper use only _foo.
// - else declare the _U prototype and mapping right after the existing undef.

intptr_t spawnve_U(int mode, const char *cmd, char *const *argv, char *const *env);
#define spawnve spawnve_U

int access_U(const char *path, int mode);
#define _access access_U
