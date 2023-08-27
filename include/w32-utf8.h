char **mu_get_utf8_argv(char **argv);
void mu_init_utf8_env(void);
void mu_export_utf8_env(void);

// the utf8 _U APIs are easily exposed, but mapping the ansi API is tricky.
// for ansi APIs which are not wrapped in mingw/winansi/etc, we simply
// map them globally as #define FuncA func_U, or #define [_]func func_U.
// But some ansi APIs are already mapped into mingw/winansi/etc, like fopen,
// and in those cases the name is typically #undef[ined] right before the
// mingw/winansi wrapper implementation (so that it can call the underlaying
// API with that name - which we want to map to the _U API), so we shouldn't
// override the name globally.
// Instead, in such cases, we declare the prototype and mapping right
// after the #existing undef of the ansi name (at the C file of the wrapper).

intptr_t spawnve_U(int mode, const char *cmd, char *const *argv, char *const *env);
#undef spawnve
#define spawnve spawnve_U

