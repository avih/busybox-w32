#include <stdio.h>
#include <windows.h>

#include "w32-utf8.h"

void *xmalloc(size_t);

// ------------------- UTF8 conversion utils -------------------

// All functions which use mu_utf8_count/mu_wide_count to first check the
// expected size assume the conversion will work with same or bigger space.

// performs a conversion, trimmed (without null) if dest size is insufficient
// if the dest size is 0: return the required size, inc \0, in dest units.
// TODO: if input size is given, does it convert beyond input \0 if size allows?
#define mu_utf8_raw(ws_src, nws, u8_dst, nu8) \
	WideCharToMultiByte(CP_UTF8, 0, (ws_src), (nws), (u8_dst), (nu8), 0, 0)
#define mu_wide_raw(u8_src, nu8, ws_dst, nws) \
	MultiByteToWideChar(CP_UTF8, 0, (u8_src), (nu8), (ws_dst), (nws))

// positive (not 0) on success. result is in destination units and includes the
// terminating null if it's within the input count range. nws/nu8 can be -1 to
// indicate that the input is null-terminated (and the result includes it).
#define mu_utf8_count(ws_src, nws) mu_utf8_raw((ws_src), (nws), 0, 0)
#define mu_wide_count(u8_src, nu8) mu_wide_raw((u8_src), (nu8), 0, 0)

// dies on OOM, returns NULL on other errors.
static char *mu_utf8(const wchar_t *ws)
{
	char *u8 = 0;
	if (ws) {
		int n = mu_utf8_count(ws, -1);
		if (n > 0) {
			u8 = xmalloc(sizeof(char) * n);
			mu_utf8_raw(ws, -1, u8, n);
		} else {
			errno = EILSEQ;
		}
	}
	return u8;
}

// dies on OOM, returns NULL on other errors.
static wchar_t *mu_wide(const char *u8)
{
	wchar_t *ws = 0;
	if (u8) {
		int n = mu_wide_count(u8, -1);
		if (n > 0) {
			ws = xmalloc(sizeof(wchar_t) * n);
			mu_wide_raw(u8, -1, ws, n);
		} else {
			errno = EILSEQ;
		}
	}
	return ws;
}

// continuous allocation for the pointers (incl. final NULL) and the strings.
// if maxn >= 0 then up to maxn elements or NULL - whichever comes first.
// final NULL is always added at the result array except if input is NULL.
// dies on OOM, returns NULL if input is NULL or if some conversion failed.
static char **mu_utf8_vec(wchar_t *const *wvec, int maxn)
{
	size_t usize, n, i;
	char **uvec, *uval;

	if (!wvec)
		return 0;

	for (usize = 0, n = 0; (maxn < 0 || n < maxn) && wvec[n]; n++) {
		int count = mu_utf8_count(wvec[n], -1);
		if (count <= 0)
			return 0;
		usize += count;
	}

	uvec = xmalloc((n+1) * sizeof(char *) + usize * sizeof(char));
	for (i = 0, uval = (void *)(uvec + n + 1); i < n; i++) {
		uvec[i] = uval;
		uval += mu_utf8_raw(wvec[i], -1, uval, usize);
	}
	uvec[i] = 0;

	return uvec;
}

static wchar_t **mu_wide_vec(char *const *uvec, int maxn)
{
	size_t wsize, n, i;
	wchar_t **wvec, *wval;

	if (!uvec)
		return 0;

	for (wsize = 0, n = 0; (maxn < 0 || n < maxn) && uvec[n]; n++) {
		int count = mu_wide_count(uvec[n], -1);
		if (count <= 0)
			return 0;
		wsize += count;
	}

	wvec = xmalloc((n+1) * sizeof(wchar_t *) + wsize * sizeof(wchar_t));
	for (i = 0, wval = (void *)(wvec + n + 1); i < n; i++) {
		wvec[i] = wval;
		wval += mu_wide_raw(uvec[i], -1, wval, wsize);
	}
	wvec[i] = 0;

	return wvec;
}

// convert u8s into wbuf if it fits + \0 in wcount, else allocate the result
static wchar_t *mu_wide_buf(const char *u8s, wchar_t *wbuf, size_t wcount)
{
	return mu_wide_raw(u8s, -1, wbuf, wcount) > 0 ? wbuf
	     : GetLastError() == ERROR_INSUFFICIENT_BUFFER ? mu_wide(u8s)
	     : (errno = EILSEQ, (wchar_t*)0);
}

// boilerplate wrapper to define + init a wchar_t* pointer from a utf8 string.
// it'll point to a local buffer if it fits, else allocated. the statement is
// then executed if there were no conversion errors (NULL u8 is not an error).
// dealloction is after the statement/block, so inner return or goto may leak.
// the macro expands to `for(...)', expecting a normal statement/block body.
// (the loop stop condition is when wvar points to itself - after 1 iteration)
// Note that the second argument (the u8 string) is evaluated more tha once.
// - on conversion failure: errno and last error are set accordingly.
// - after the user statement: none touched, except errno if `free' failed.
// e.g.:
//   IF_WITH_WSTR(wfoo, u8foo)
//       IF_WITH_WSTR(wbar, u8bar)
//           // use wfoo, wbar. they'll be freed afterwards if needed
#define IF_WITH_WSTR(wvar, u8) \
	for (wchar_t wvar##_buf[128], *wvar = mu_wide_buf(u8, wvar##_buf, 128); \
	     (wvar || !u8) && wvar != (void*)&wvar; \
	     wvar != wvar##_buf ? free(wvar), 0 : 0, wvar = (void*)&wvar)

// same as IF_WITH_WSTR, but possibly with extra sauce for paths/filenames
#define IF_WITH_WPATH(wvar, u8) IF_WITH_WSTR(wvar, u8)


// ensure argv and environ are UT8
char **mu_get_utf8_argv(char **argv)
{
	int n;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &n);
	char **u8argv = mu_utf8_vec(wargv, n);

	if (wargv)
		LocalFree(wargv);

	return u8argv ? u8argv : argv;
}

// [ wide proc env - accessed via {Get,Set}EnvironmentVariableW et al ]
// [ ANSI CRT env - environ - {get,put}env..., copied from the proc on init ]
// [ the CRT also has a wide _wenviron, but it remains NULL in busybox-w32 ]
// [ updating the CRT env also updates the proc env, but not the other way ]
//
// this is a bit hacky, but it works to initialize the utf8 env:
// for every wide proc env var with ascii-name and non-ascii value, we update
// this var at the CRT ANSI environ to the utf8 value. This works because
// putenv allows/takes arbitrary ANSI CP (ACP) values, seemingly even in DBCS.
// (busybox can't touch non-ascii env names anyway, so they remain unmodified)
//
// this, however, also updates the corresponding wide proc env "accordingly",
// but it assumes the CRT env is encoded in ACP, and so the wide proc env value
// becomes broken - but the CRT ANSI env is still the correct utf8 value.
// the same also happens later with putenv of of non-ascii utf8 values.
//
// the only place this becomes an issue is when spawning a process with
// NULL (w)env arg, which means it should take the (now broken) wide proc env.
// so in this case, we export the utf8 values back to the wide proc env.
void mu_init_utf8_env(void)
{
	wchar_t *envw0 = GetEnvironmentStringsW(), *envw = envw0, *p;

	for (char *eu; envw && *(p = envw); envw += wcslen(envw) + 1) {
		while (*p && *p != '=' && *p < 0x80)
			++p;
		if (*p++ != '=')
			continue;  // non-ascii7 name, or no '='

		while (*p && *p < 0x80)
			++p;
		if (!*p)
			continue;

		if ((eu = mu_utf8(envw))) {
			// ascii7 name, unicode value, and converted
			_putenv(eu);
			free(eu);  // windows putenv makes a copy
		}
	}

	FreeEnvironmentStringsW(envw0);
}

// for any ascii7 var name with non-ascii utf8 value at environ, set the
// system wvar with the unicode value (the crt _[w]environ are unmodified)
void mu_export_utf8_env(void)
{
	for (char *p, **env = environ; env && (p = *env); ++env) {
		while (*p && *p != '=' && (unsigned char)*p < 0x80)
			++p;
		if (*p++ != '=')
			continue;

		while (*p && (unsigned char)*p < 0x80)
			++p;
		if (!*p)
			continue;

		// ascii7 name, unicode value
		IF_WITH_WSTR(wenv, *env) {
			wchar_t *weq = wcschr(wenv, '=');
			if (weq) {
				*weq = 0;
				SetEnvironmentVariableW(wenv, weq + 1);
			}
		}
	}
}

intptr_t spawnve_U(int mode, const char *cmd, char *const *argv, char *const *env)
{
	intptr_t ret = -1;
	wchar_t *wcmd = mu_wide(cmd),
	        **wargv = mu_wide_vec(argv, -1),
	        **wenv = mu_wide_vec(env, -1);

	if ((cmd && !wcmd) || (argv && !wargv) || (env && !wenv)) {
		errno = EINVAL;
	} else {
		if (!env)  // env will be the system's - ensure it's up to date
			mu_export_utf8_env();
		ret = _wspawnve(mode, wcmd, (const wchar_t *const *)wargv,
		                            (const wchar_t *const *)wenv);
	}

	free(wenv);
	free(wargv);
	free(wcmd);

	return ret;
}
