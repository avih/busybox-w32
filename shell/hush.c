/* vi: set sw=4 ts=4: */
/*
 * sh.c -- a prototype Bourne shell grammar parser
 *      Intended to follow the original Thompson and Ritchie
 *      "small and simple is beautiful" philosophy, which
 *      incidentally is a good match to today's BusyBox.
 *
 * Copyright (C) 2000,2001  Larry Doolittle  <larry@doolittle.boa.org>
 *
 * Credits:
 *      The parser routines proper are all original material, first
 *      written Dec 2000 and Jan 2001 by Larry Doolittle.  The
 *      execution engine, the builtins, and much of the underlying
 *      support has been adapted from busybox-0.49pre's lash, which is
 *      Copyright (C) 1999-2004 by Erik Andersen <andersen@codepoet.org>
 *      written by Erik Andersen <andersen@codepoet.org>.  That, in turn,
 *      is based in part on ladsh.c, by Michael K. Johnson and Erik W.
 *      Troan, which they placed in the public domain.  I don't know
 *      how much of the Johnson/Troan code has survived the repeated
 *      rewrites.
 *
 * Other credits:
 *      b_addchr() derived from similar w_addchar function in glibc-2.2
 *      setup_redirect(), redirect_opt_num(), and big chunks of main()
 *      and many builtins derived from contributions by Erik Andersen
 *      miscellaneous bugfixes from Matt Kraai
 *
 * There are two big (and related) architecture differences between
 * this parser and the lash parser.  One is that this version is
 * actually designed from the ground up to understand nearly all
 * of the Bourne grammar.  The second, consequential change is that
 * the parser and input reader have been turned inside out.  Now,
 * the parser is in control, and asks for input as needed.  The old
 * way had the input reader in control, and it asked for parsing to
 * take place as needed.  The new way makes it much easier to properly
 * handle the recursion implicit in the various substitutions, especially
 * across continuation lines.
 *
 * Bash grammar not implemented: (how many of these were in original sh?)
 *      $@ (those sure look like weird quoting rules)
 *      $_
 *      ! negation operator for pipes
 *      &> and >& redirection of stdout+stderr
 *      Brace Expansion
 *      Tilde Expansion
 *      fancy forms of Parameter Expansion
 *      aliases
 *      Arithmetic Expansion
 *      <(list) and >(list) Process Substitution
 *      reserved words: case, esac, select, function
 *      Here Documents ( << word )
 *      Functions
 * Major bugs:
 *      job handling woefully incomplete and buggy (improved --vda)
 *      reserved word execution woefully incomplete and buggy
 * to-do:
 *      port selected bugfixes from post-0.49 busybox lash - done?
 *      finish implementing reserved words: for, while, until, do, done
 *      change { and } from special chars to reserved words
 *      builtins: break, continue, eval, return, set, trap, ulimit
 *      test magic exec
 *      handle children going into background
 *      clean up recognition of null pipes
 *      check setting of global_argc and global_argv
 *      control-C handling, probably with longjmp
 *      follow IFS rules more precisely, including update semantics
 *      figure out what to do with backslash-newline
 *      explain why we use signal instead of sigaction
 *      propagate syntax errors, die on resource errors?
 *      continuation lines, both explicit and implicit - done?
 *      memory leak finding and plugging - done?
 *      more testing, especially quoting rules and redirection
 *      document how quoting rules not precisely followed for variable assignments
 *      maybe change map[] to use 2-bit entries
 *      (eventually) remove all the printf's
 *
 * Licensed under the GPL v2 or later, see the file LICENSE in this tarball.
 */

#include "busybox.h"
#include <glob.h>      /* glob, of course */
#include <getopt.h>    /* should be pretty obvious */

/* #include <dmalloc.h> */
//#define DEBUG_SHELL
/* Finer-grained debug switch */
//#define DEBUG_SHELL_JOBS

//TODO: rename HUSH_INTERACTIVE -> HUSH_JOB,
//create HUSH_INTERACTIVE which controls only prompt + line editing,
//make HUSH_JOB dependent on it

#if !ENABLE_HUSH_INTERACTIVE
#undef ENABLE_FEATURE_EDITING
#define ENABLE_FEATURE_EDITING 0
#undef ENABLE_FEATURE_EDITING_FANCY_PROMPT
#define ENABLE_FEATURE_EDITING_FANCY_PROMPT 0
#endif

#define SPECIAL_VAR_SYMBOL 03
#define FLAG_EXIT_FROM_LOOP 1
#define FLAG_PARSE_SEMICOLON (1 << 1)		/* symbol ';' is special for parser */
#define FLAG_REPARSING	     (1 << 2)		/* >=2nd pass */

typedef enum {
	REDIRECT_INPUT     = 1,
	REDIRECT_OVERWRITE = 2,
	REDIRECT_APPEND    = 3,
	REDIRECT_HEREIS    = 4,
	REDIRECT_IO        = 5
} redir_type;

/* The descrip member of this structure is only used to make debugging
 * output pretty */
static const struct {
	int mode;
	int default_fd;
	const char *descrip;
} redir_table[] = {
	{ 0,                         0, "()" },
	{ O_RDONLY,                  0, "<"  },
	{ O_CREAT|O_TRUNC|O_WRONLY,  1, ">"  },
	{ O_CREAT|O_APPEND|O_WRONLY, 1, ">>" },
	{ O_RDONLY,                 -1, "<<" },
	{ O_RDWR,                    1, "<>" }
};

typedef enum {
	PIPE_SEQ = 1,
	PIPE_AND = 2,
	PIPE_OR  = 3,
	PIPE_BG  = 4,
} pipe_style;

/* might eventually control execution */
typedef enum {
	RES_NONE  = 0,
	RES_IF    = 1,
	RES_THEN  = 2,
	RES_ELIF  = 3,
	RES_ELSE  = 4,
	RES_FI    = 5,
	RES_FOR   = 6,
	RES_WHILE = 7,
	RES_UNTIL = 8,
	RES_DO    = 9,
	RES_DONE  = 10,
	RES_XXXX  = 11,
	RES_IN    = 12,
	RES_SNTX  = 13
} reserved_style;
enum {
	FLAG_END   = (1 << RES_NONE ),
	FLAG_IF    = (1 << RES_IF   ),
	FLAG_THEN  = (1 << RES_THEN ),
	FLAG_ELIF  = (1 << RES_ELIF ),
	FLAG_ELSE  = (1 << RES_ELSE ),
	FLAG_FI    = (1 << RES_FI   ),
	FLAG_FOR   = (1 << RES_FOR  ),
	FLAG_WHILE = (1 << RES_WHILE),
	FLAG_UNTIL = (1 << RES_UNTIL),
	FLAG_DO    = (1 << RES_DO   ),
	FLAG_DONE  = (1 << RES_DONE ),
	FLAG_IN    = (1 << RES_IN   ),
	FLAG_START = (1 << RES_XXXX ),
};

/* This holds pointers to the various results of parsing */
struct p_context {
	struct child_prog *child;
	struct pipe *list_head;
	struct pipe *pipe;
	struct redir_struct *pending_redirect;
	reserved_style w;
	int old_flag;               /* for figuring out valid reserved words */
	struct p_context *stack;
	int type;           /* define type of parser : ";$" common or special symbol */
	/* How about quoting status? */
};

struct redir_struct {
	struct redir_struct *next;  /* pointer to the next redirect in the list */
	redir_type type;            /* type of redirection */
	int fd;                     /* file descriptor being redirected */
	int dup;                    /* -1, or file descriptor being duplicated */
	glob_t word;                /* *word.gl_pathv is the filename */
};

struct child_prog {
	pid_t pid;                  /* 0 if exited */
	char **argv;                /* program name and arguments */
	struct pipe *group;         /* if non-NULL, first in group or subshell */
	int subshell;               /* flag, non-zero if group must be forked */
	struct redir_struct *redirects; /* I/O redirections */
	glob_t glob_result;         /* result of parameter globbing */
	int is_stopped;             /* is the program currently running? */
	struct pipe *family;        /* pointer back to the child's parent pipe */
	int sp;                     /* number of SPECIAL_VAR_SYMBOL */
	int type;
};

struct pipe {
	struct pipe *next;
	int num_progs;              /* total number of programs in job */
	int running_progs;          /* number of programs running (not exited) */
	char *cmdbuf;               /* buffer various argv's point into */
#if ENABLE_HUSH_INTERACTIVE
	int jobid;                  /* job number */
	char *cmdtext;              /* name of job */
	pid_t pgrp;                 /* process group ID for the job */
#endif
	struct child_prog *progs;   /* array of commands in pipe */
	int stopped_progs;          /* number of programs alive, but stopped */
	int job_context;            /* bitmask defining current context */
	pipe_style followup;        /* PIPE_BG, PIPE_SEQ, PIPE_OR, PIPE_AND */
	reserved_style r_mode;      /* supports if, for, while, until */
};

struct close_me {
	struct close_me *next;
	int fd;
};

struct variables {
	struct variables *next;
	const char *name;
	const char *value;
	int flg_export;
	int flg_read_only;
};

/* globals, connect us to the outside world
 * the first three support $?, $#, and $1 */
static char **global_argv;
static int global_argc;
static int last_return_code;
extern char **environ; /* This is in <unistd.h>, but protected with __USE_GNU */

/* "globals" within this file */
static const char *ifs;
static unsigned char map[256];
static int fake_mode;
static struct close_me *close_me_head;
static const char *cwd;
static unsigned last_bg_pid;
#if ENABLE_HUSH_INTERACTIVE
static int last_jobid;
static struct pipe *job_list;
/* 'interactive_fd' is a fd# open to ctty, if we have one
 * _AND_ if we decided to mess with job control */
static int interactive_fd;
static pid_t saved_task_pgrp;
static pid_t saved_tty_pgrp;
#else
enum { interactive_fd = 0 };
#endif

static const char *PS1;
static const char *PS2;
static struct variables shell_ver = { NULL, "HUSH_VERSION", "0.01", 1, 1 };
static struct variables *top_vars = &shell_ver;


#define B_CHUNK (100)
#define B_NOSPAC 1

typedef struct {
	char *data;
	int length;
	int maxlen;
	int quote;
	int nonnull;
} o_string;
#define NULL_O_STRING {NULL,0,0,0,0}
/* used for initialization:
	o_string foo = NULL_O_STRING; */

/* I can almost use ordinary FILE *.  Is open_memstream() universally
 * available?  Where is it documented? */
struct in_str {
	const char *p;
	char peek_buf[2];
	int __promptme;
	int promptmode;
	FILE *file;
	int (*get) (struct in_str *);
	int (*peek) (struct in_str *);
};
#define b_getch(input) ((input)->get(input))
#define b_peek(input) ((input)->peek(input))

#define JOB_STATUS_FORMAT "[%d] %-22s %.40s\n"

struct built_in_command {
	const char *cmd;                /* name */
	const char *descr;              /* description */
	int (*function) (char **argv);  /* function ptr */
};

/* belongs in busybox.h */
static int max(int a, int b)
{
	return (a > b) ? a : b;
}

#ifdef DEBUG_SHELL
#define debug_printf(...) fprintf(stderr, __VA_ARGS__)
/* broken, of course, but OK for testing */
static char *indenter(int i)
{
	static char blanks[] = "                                    ";
	return &blanks[sizeof(blanks) - i - 1];
}
#else
#define debug_printf(...) do {} while (0)
#endif

#ifdef DEBUG_SHELL_JOBS
#define debug_jobs_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define debug_jobs_printf(...) do {} while (0)
#endif

#define final_printf debug_printf

static void __syntax(const char *file, int line)
{
	bb_error_msg("syntax error %s:%d", file, line);
}
/* NB: was __FILE__, but that produces full path sometimes, so... */
#define syntax() __syntax("hush.c", __LINE__)

/* Index of subroutines: */
/*   function prototypes for builtins */
static int builtin_cd(char **argv);
static int builtin_env(char **argv);
static int builtin_eval(char **argv);
static int builtin_exec(char **argv);
static int builtin_exit(char **argv);
static int builtin_export(char **argv);
#if ENABLE_HUSH_INTERACTIVE
static int builtin_fg_bg(char **argv);
static int builtin_jobs(char **argv);
#endif
static int builtin_help(char **argv);
static int builtin_pwd(char **argv);
static int builtin_read(char **argv);
static int builtin_set(char **argv);
static int builtin_shift(char **argv);
static int builtin_source(char **argv);
static int builtin_umask(char **argv);
static int builtin_unset(char **argv);
static int builtin_not_written(char **argv);
/*   o_string manipulation: */
static int b_check_space(o_string *o, int len);
static int b_addchr(o_string *o, int ch);
static void b_reset(o_string *o);
static int b_addqchr(o_string *o, int ch, int quote);
static int b_adduint(o_string *o, unsigned i);
/*  in_str manipulations: */
static int static_get(struct in_str *i);
static int static_peek(struct in_str *i);
static int file_get(struct in_str *i);
static int file_peek(struct in_str *i);
static void setup_file_in_str(struct in_str *i, FILE *f);
static void setup_string_in_str(struct in_str *i, const char *s);
/*  close_me manipulations: */
static void mark_open(int fd);
static void mark_closed(int fd);
static void close_all(void);
/*  "run" the final data structures: */
static int free_pipe_list(struct pipe *head, int indent);
static int free_pipe(struct pipe *pi, int indent);
/*  really run the final data structures: */
static int setup_redirects(struct child_prog *prog, int squirrel[]);
static int run_list_real(struct pipe *pi);
static void pseudo_exec_argv(char **argv) ATTRIBUTE_NORETURN;
static void pseudo_exec(struct child_prog *child) ATTRIBUTE_NORETURN;
static int run_pipe_real(struct pipe *pi);
/*   extended glob support: */
static int globhack(const char *src, int flags, glob_t *pglob);
static int glob_needed(const char *s);
static int xglob(o_string *dest, int flags, glob_t *pglob);
/*   variable assignment: */
static int is_assignment(const char *s);
/*   data structure manipulation: */
static int setup_redirect(struct p_context *ctx, int fd, redir_type style, struct in_str *input);
static void initialize_context(struct p_context *ctx);
static int done_word(o_string *dest, struct p_context *ctx);
static int done_command(struct p_context *ctx);
static int done_pipe(struct p_context *ctx, pipe_style type);
/*   primary string parsing: */
static int redirect_dup_num(struct in_str *input);
static int redirect_opt_num(o_string *o);
static int process_command_subs(o_string *dest, struct p_context *ctx, struct in_str *input, int subst_end);
static int parse_group(o_string *dest, struct p_context *ctx, struct in_str *input, int ch);
static const char *lookup_param(const char *src);
static char *make_string(char **inp);
static int handle_dollar(o_string *dest, struct p_context *ctx, struct in_str *input);
static int parse_string(o_string *dest, struct p_context *ctx, const char *src);
static int parse_stream(o_string *dest, struct p_context *ctx, struct in_str *input0, int end_trigger);
/*   setup: */
static int parse_stream_outer(struct in_str *inp, int flag);
static int parse_string_outer(const char *s, int flag);
static int parse_file_outer(FILE *f);
/*   job management: */
static int checkjobs(struct pipe* fg_pipe);
#if ENABLE_HUSH_INTERACTIVE
static int checkjobs_and_fg_shell(struct pipe* fg_pipe);
static void insert_bg_job(struct pipe *pi);
static void remove_bg_job(struct pipe *pi);
static void delete_finished_bg_job(struct pipe *pi);
#else
int checkjobs_and_fg_shell(struct pipe* fg_pipe); /* never called */
#endif
/*     local variable support */
static char **make_list_in(char **inp, char *name);
static char *insert_var_value(char *inp);
static const char *get_local_var(const char *var);
static int set_local_var(const char *s, int flg_export);
static void unset_local_var(const char *name);

/* Table of built-in functions.  They can be forked or not, depending on
 * context: within pipes, they fork.  As simple commands, they do not.
 * When used in non-forking context, they can change global variables
 * in the parent shell process.  If forked, of course they cannot.
 * For example, 'unset foo | whatever' will parse and run, but foo will
 * still be set at the end. */
static const struct built_in_command bltins[] = {
#if ENABLE_HUSH_INTERACTIVE
	{ "bg", "Resume a job in the background", builtin_fg_bg },
#endif
	{ "break", "Exit for, while or until loop", builtin_not_written },
	{ "cd", "Change working directory", builtin_cd },
	{ "continue", "Continue for, while or until loop", builtin_not_written },
	{ "env", "Print all environment variables", builtin_env },
	{ "eval", "Construct and run shell command", builtin_eval },
	{ "exec", "Exec command, replacing this shell with the exec'd process",
		builtin_exec },
	{ "exit", "Exit from shell()", builtin_exit },
	{ "export", "Set environment variable", builtin_export },
#if ENABLE_HUSH_INTERACTIVE
	{ "fg", "Bring job into the foreground", builtin_fg_bg },
	{ "jobs", "Lists the active jobs", builtin_jobs },
#endif
	{ "pwd", "Print current directory", builtin_pwd },
	{ "read", "Input environment variable", builtin_read },
	{ "return", "Return from a function", builtin_not_written },
	{ "set", "Set/unset shell local variables", builtin_set },
	{ "shift", "Shift positional parameters", builtin_shift },
	{ "trap", "Trap signals", builtin_not_written },
	{ "ulimit","Controls resource limits", builtin_not_written },
	{ "umask","Sets file creation mask", builtin_umask },
	{ "unset", "Unset environment variable", builtin_unset },
	{ ".", "Source-in and run commands in a file", builtin_source },
	{ "help", "List shell built-in commands", builtin_help },
	{ NULL, NULL, NULL }
};

#if ENABLE_HUSH_INTERACTIVE

#if ENABLE_FEATURE_SH_STANDALONE
/* move to libbb? */
static void signal_SA_RESTART(int sig, void (*handler)(int))
{
	struct sigaction sa;
	sa.sa_handler = handler;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}
#endif

/* Signals are grouped, we handle them in batches */
static void set_fatal_sighandler(void (*handler)(int))
{
	signal(SIGILL , handler);
	signal(SIGTRAP, handler);
	signal(SIGABRT, handler);
	signal(SIGFPE , handler);
	signal(SIGBUS , handler);
	signal(SIGSEGV, handler);
	/* bash 3.2 seems to handle these just like 'fatal' ones */
	signal(SIGHUP , handler);
	signal(SIGPIPE, handler);
	signal(SIGALRM, handler);
}
static void set_jobctrl_sighandler(void (*handler)(int))
{
	signal(SIGTSTP, handler);
	signal(SIGTTIN, handler);
	signal(SIGTTOU, handler);
}
static void set_misc_sighandler(void (*handler)(int))
{
	signal(SIGINT , handler);
	signal(SIGQUIT, handler);
	signal(SIGTERM, handler);
}
/* SIGCHLD is special and handled separately */

#if ENABLE_FEATURE_SH_STANDALONE
static void set_every_sighandler(void (*handler)(int))
{
	set_fatal_sighandler(handler);
	set_jobctrl_sighandler(handler);
	set_misc_sighandler(handler);
	signal(SIGCHLD, handler);
}

static struct pipe *nofork_pipe;
struct nofork_save_area nofork_save;
static sigjmp_buf nofork_jb;

static void handler_ctrl_c(int sig)
{
	debug_jobs_printf("got sig %d\n", sig);
// as usual we can have all kinds of nasty problems with leaked malloc data here
	siglongjmp(nofork_jb, 1);
}

static void handler_ctrl_z(int sig)
{
	pid_t pid;

	debug_jobs_printf("got tty sig %d\n", sig);
	pid = fork();
	if (pid < 0) /* can't fork. Pretend there were no Ctrl-Z */
		return;
	debug_jobs_printf("bg'ing nofork\n");
	nofork_save.saved = 0; /* flag the fact that Ctrl-Z was handled */
	nofork_pipe->running_progs = 1;
	nofork_pipe->stopped_progs = 0;
	if (!pid) { /* child */
		debug_jobs_printf("setting pgrp for child\n");
		setpgrp();
		set_every_sighandler(SIG_DFL);
		raise(SIGTSTP); /* resend TSTP so that child will be stopped */
		debug_jobs_printf("returning to child\n");
		/* return to nofork, it will eventually exit now,
		 * not return back to shell */
		return;
	}
	/* parent */
	/* finish filling up pipe info */
	nofork_pipe->pgrp = pid; /* child is in its own pgrp */
	nofork_pipe->progs[0].pid = pid;
	nofork_pipe->running_progs = 1;
	nofork_pipe->stopped_progs = 0;
	/* parent needs to longjmp out of running nofork.
	 * we will "return" exitcode 0, with child put in background */
// as usual we can have all kinds of nasty problems with leaked malloc data here
	siglongjmp(nofork_jb, 1);
}

#endif

/* Restores tty foreground process group, and exits.
 * May be called as signal handler for fatal signal
 * (will faithfully resend signal to itself, producing correct exit state)
 * or called directly with -EXITCODE.
 * We also call it if xfunc is exiting. */
static void sigexit(int sig) ATTRIBUTE_NORETURN;
static void sigexit(int sig)
{
	sigset_t block_all;

	/* Disable all signals: job control, SIGPIPE, etc. */
	sigfillset(&block_all);
	sigprocmask(SIG_SETMASK, &block_all, NULL);

	if (interactive_fd)
		tcsetpgrp(interactive_fd, saved_tty_pgrp);

	/* Not a signal, just exit */
	if (sig <= 0)
		_exit(- sig);

	/* Enable only this sig and kill ourself with it */
	signal(sig, SIG_DFL);
	sigdelset(&block_all, sig);
	sigprocmask(SIG_SETMASK, &block_all, NULL);
	raise(sig);
	_exit(1); /* Should not reach it */
}

/* Restores tty foreground process group, and exits. */
static void hush_exit(int exitcode) ATTRIBUTE_NORETURN;
static void hush_exit(int exitcode)
{
	fflush(NULL); /* flush all streams */
	sigexit(- (exitcode & 0xff));
}

#else /* !INTERACTIVE */

#define set_fatal_sighandler(handler)   ((void)0)
#define set_jobctrl_sighandler(handler) ((void)0)
#define set_misc_sighandler(handler)    ((void)0)
#define hush_exit(e)                    exit(-(e))

#endif /* INTERACTIVE */


static const char *set_cwd(void)
{
	if (cwd == bb_msg_unknown)
		cwd = NULL;     /* xrealloc_getcwd_or_warn(arg) calls free(arg)! */
	cwd = xrealloc_getcwd_or_warn((char *)cwd);
	if (!cwd)
		cwd = bb_msg_unknown;
	return cwd;
}

/* built-in 'eval' handler */
static int builtin_eval(char **argv)
{
	char *str = NULL;
	int rcode = EXIT_SUCCESS;

	if (argv[1]) {
		str = make_string(argv + 1);
		parse_string_outer(str, FLAG_EXIT_FROM_LOOP |
					FLAG_PARSE_SEMICOLON);
		free(str);
		rcode = last_return_code;
	}
	return rcode;
}

/* built-in 'cd <path>' handler */
static int builtin_cd(char **argv)
{
	char *newdir;
	if (argv[1] == NULL)
		newdir = getenv("HOME");
	else
		newdir = argv[1];
	if (chdir(newdir)) {
		printf("cd: %s: %s\n", newdir, strerror(errno));
		return EXIT_FAILURE;
	}
	set_cwd();
	return EXIT_SUCCESS;
}

/* built-in 'env' handler */
static int builtin_env(char **argv ATTRIBUTE_UNUSED)
{
/* TODO: call env applet's code instead */
	char **e = environ;
	if (e == NULL)
		return EXIT_FAILURE;
	while (*e) {
		puts(*e++);
	}
	return EXIT_SUCCESS;
}

/* built-in 'exec' handler */
static int builtin_exec(char **argv)
{
	if (argv[1] == NULL)
		return EXIT_SUCCESS;   /* Really? */
	pseudo_exec_argv(argv + 1);
	/* never returns */
}

/* built-in 'exit' handler */
static int builtin_exit(char **argv)
{
// TODO: bash does it ONLY on top-level sh exit (+interacive only?)
	//puts("exit"); /* bash does it */

	if (argv[1] == NULL)
		hush_exit(last_return_code);
	/* mimic bash: exit 123abc == exit 255 + error msg */
	xfunc_error_retval = 255;
	/* bash: exit -2 == exit 254, no error msg */
	hush_exit(xatoi(argv[1]));
}

/* built-in 'export VAR=value' handler */
static int builtin_export(char **argv)
{
	int res = 0;
	char *name = argv[1];

	if (name == NULL) {
		return builtin_env(argv);
	}

	name = strdup(name);

	if (name) {
		const char *value = strchr(name, '=');

		if (!value) {
			char *tmp;
			/* They are exporting something without an =VALUE */

			value = get_local_var(name);
			if (value) {
				size_t ln = strlen(name);

				tmp = realloc(name, ln+strlen(value)+2);
				if (tmp == NULL)
					res = -1;
				else {
					sprintf(tmp+ln, "=%s", value);
					name = tmp;
				}
			} else {
				/* bash does not return an error when trying to export
				 * an undefined variable.  Do likewise. */
				res = 1;
			}
		}
	}
	if (res < 0)
		bb_perror_msg("export");
	else if (res == 0)
		res = set_local_var(name, 1);
	else
		res = 0;
	free(name);
	return res;
}

#if ENABLE_HUSH_INTERACTIVE
/* built-in 'fg' and 'bg' handler */
static int builtin_fg_bg(char **argv)
{
	int i, jobnum;
	struct pipe *pi;

	if (!interactive_fd)
		return EXIT_FAILURE;
	/* If they gave us no args, assume they want the last backgrounded task */
	if (!argv[1]) {
		for (pi = job_list; pi; pi = pi->next) {
			if (pi->jobid == last_jobid) {
				goto found;
			}
		}
		bb_error_msg("%s: no current job", argv[0]);
		return EXIT_FAILURE;
	}
	if (sscanf(argv[1], "%%%d", &jobnum) != 1) {
		bb_error_msg("%s: bad argument '%s'", argv[0], argv[1]);
		return EXIT_FAILURE;
	}
	for (pi = job_list; pi; pi = pi->next) {
		if (pi->jobid == jobnum) {
			goto found;
		}
	}
	bb_error_msg("%s: %d: no such job", argv[0], jobnum);
	return EXIT_FAILURE;
 found:
	// TODO: bash prints a string representation
	// of job being foregrounded (like "sleep 1 | cat")
	if (*argv[0] == 'f') {
		/* Put the job into the foreground.  */
		tcsetpgrp(interactive_fd, pi->pgrp);
	}

	/* Restart the processes in the job */
	debug_jobs_printf("reviving %d procs, pgrp %d\n", pi->num_progs, pi->pgrp);
	for (i = 0; i < pi->num_progs; i++) {
		debug_jobs_printf("reviving pid %d\n", pi->progs[i].pid);
		pi->progs[i].is_stopped = 0;
	}
	pi->stopped_progs = 0;

	i = kill(- pi->pgrp, SIGCONT);
	if (i < 0) {
		if (errno == ESRCH) {
			delete_finished_bg_job(pi);
			return EXIT_SUCCESS;
		} else {
			bb_perror_msg("kill (SIGCONT)");
		}
	}

	if (*argv[0] == 'f') {
		remove_bg_job(pi);
		return checkjobs_and_fg_shell(pi);
	}
	return EXIT_SUCCESS;
}
#endif

/* built-in 'help' handler */
static int builtin_help(char **argv ATTRIBUTE_UNUSED)
{
	const struct built_in_command *x;

	printf("\nBuilt-in commands:\n");
	printf("-------------------\n");
	for (x = bltins; x->cmd; x++) {
		if (x->descr == NULL)
			continue;
		printf("%s\t%s\n", x->cmd, x->descr);
	}
	printf("\n\n");
	return EXIT_SUCCESS;
}

#if ENABLE_HUSH_INTERACTIVE
/* built-in 'jobs' handler */
static int builtin_jobs(char **argv ATTRIBUTE_UNUSED)
{
	struct pipe *job;
	const char *status_string;

	for (job = job_list; job; job = job->next) {
		if (job->running_progs == job->stopped_progs)
			status_string = "Stopped";
		else
			status_string = "Running";

		printf(JOB_STATUS_FORMAT, job->jobid, status_string, job->cmdtext);
	}
	return EXIT_SUCCESS;
}
#endif

/* built-in 'pwd' handler */
static int builtin_pwd(char **argv ATTRIBUTE_UNUSED)
{
	puts(set_cwd());
	return EXIT_SUCCESS;
}

/* built-in 'read VAR' handler */
static int builtin_read(char **argv)
{
	int res;

	if (argv[1]) {
		char string[BUFSIZ];
		char *var = NULL;

		string[0] = '\0';  /* In case stdin has only EOF */
		/* read string */
		fgets(string, sizeof(string), stdin);
		chomp(string);
		var = malloc(strlen(argv[1]) + strlen(string) + 2);
		if (var) {
			sprintf(var, "%s=%s", argv[1], string);
			res = set_local_var(var, 0);
		} else
			res = -1;
		if (res)
			bb_perror_msg("read");
		free(var);      /* So not move up to avoid breaking errno */
		return res;
	}
	do res = getchar(); while (res != '\n' && res != EOF);
	return 0;
}

/* built-in 'set VAR=value' handler */
static int builtin_set(char **argv)
{
	char *temp = argv[1];
	struct variables *e;

	if (temp == NULL)
		for (e = top_vars; e; e = e->next)
			printf("%s=%s\n", e->name, e->value);
	else
		set_local_var(temp, 0);

	return EXIT_SUCCESS;
}


/* Built-in 'shift' handler */
static int builtin_shift(char **argv)
{
	int n = 1;
	if (argv[1]) {
		n = atoi(argv[1]);
	}
	if (n >= 0 && n < global_argc) {
		/* XXX This probably breaks $0 */
		global_argc -= n;
		global_argv += n;
		return EXIT_SUCCESS;
	}
	return EXIT_FAILURE;
}

/* Built-in '.' handler (read-in and execute commands from file) */
static int builtin_source(char **argv)
{
	FILE *input;
	int status;

	if (argv[1] == NULL)
		return EXIT_FAILURE;

	/* XXX search through $PATH is missing */
	input = fopen(argv[1], "r");
	if (!input) {
		bb_error_msg("cannot open '%s'", argv[1]);
		return EXIT_FAILURE;
	}

	/* Now run the file */
	/* XXX argv and argc are broken; need to save old global_argv
	 * (pointer only is OK!) on this stack frame,
	 * set global_argv=argv+1, recurse, and restore. */
	mark_open(fileno(input));
	status = parse_file_outer(input);
	mark_closed(fileno(input));
	fclose(input);
	return status;
}

static int builtin_umask(char **argv)
{
	mode_t new_umask;
	const char *arg = argv[1];
	char *end;
	if (arg) {
		new_umask = strtoul(arg, &end, 8);
		if (*end != '\0' || end == arg) {
			return EXIT_FAILURE;
		}
	} else {
		new_umask = umask(0);
		printf("%.3o\n", (unsigned) new_umask);
	}
	umask(new_umask);
	return EXIT_SUCCESS;
}

/* built-in 'unset VAR' handler */
static int builtin_unset(char **argv)
{
	/* bash returned already true */
	unset_local_var(argv[1]);
	return EXIT_SUCCESS;
}

static int builtin_not_written(char **argv)
{
	printf("builtin_%s not written\n", argv[0]);
	return EXIT_FAILURE;
}

static int b_check_space(o_string *o, int len)
{
	/* It would be easy to drop a more restrictive policy
	 * in here, such as setting a maximum string length */
	if (o->length + len > o->maxlen) {
		char *old_data = o->data;
		/* assert(data == NULL || o->maxlen != 0); */
		o->maxlen += max(2*len, B_CHUNK);
		o->data = realloc(o->data, 1 + o->maxlen);
		if (o->data == NULL) {
			free(old_data);
		}
	}
	return o->data == NULL;
}

static int b_addchr(o_string *o, int ch)
{
	debug_printf("b_addchr: %c %d %p\n", ch, o->length, o);
	if (b_check_space(o, 1))
		return B_NOSPAC;
	o->data[o->length] = ch;
	o->length++;
	o->data[o->length] = '\0';
	return 0;
}

static void b_reset(o_string *o)
{
	o->length = 0;
	o->nonnull = 0;
	if (o->data != NULL)
		*o->data = '\0';
}

static void b_free(o_string *o)
{
	b_reset(o);
	free(o->data);
	o->data = NULL;
	o->maxlen = 0;
}

/* My analysis of quoting semantics tells me that state information
 * is associated with a destination, not a source.
 */
static int b_addqchr(o_string *o, int ch, int quote)
{
	if (quote && strchr("*?[\\", ch)) {
		int rc;
		rc = b_addchr(o, '\\');
		if (rc)
			return rc;
	}
	return b_addchr(o, ch);
}

static int b_adduint(o_string *o, unsigned i)
{
	int r;
	char buf[sizeof(unsigned)*3 + 1];
	char *p = buf;
	*(utoa_to_buf(i, buf, sizeof(buf))) = '\0';
	/* no escape checking necessary */
	do r = b_addchr(o, *p++); while (r == 0 && *p);
	return r;
}

static int static_get(struct in_str *i)
{
	int ch = *i->p++;
	if (ch == '\0') return EOF;
	return ch;
}

static int static_peek(struct in_str *i)
{
	return *i->p;
}

static void cmdedit_set_initial_prompt(void)
{
#if !ENABLE_FEATURE_EDITING_FANCY_PROMPT
	PS1 = NULL;
#else
	PS1 = getenv("PS1");
	if (PS1 == NULL)
		PS1 = "\\w \\$ ";
#endif
}

static const char* setup_prompt_string(int promptmode)
{
	const char *prompt_str;
	debug_printf("setup_prompt_string %d ", promptmode);
#if !ENABLE_FEATURE_EDITING_FANCY_PROMPT
	/* Set up the prompt */
	if (promptmode == 1) {
		char *ns;
		free((char*)PS1);
		ns = xmalloc(strlen(cwd)+4);
		sprintf(ns, "%s %s", cwd, (geteuid() != 0) ? "$ " : "# ");
		prompt_str = ns;
		PS1 = ns;
	} else {
		prompt_str = PS2;
	}
#else
	prompt_str = (promptmode == 1) ? PS1 : PS2;
#endif
	debug_printf("result %s\n", prompt_str);
	return prompt_str;
}

#if ENABLE_FEATURE_EDITING
static line_input_t *line_input_state;
#endif

static int get_user_input(struct in_str *i)
{
	int r;
	const char *prompt_str;
	static char the_command[BUFSIZ];

	prompt_str = setup_prompt_string(i->promptmode);
#if ENABLE_FEATURE_EDITING
	/*
	 ** enable command line editing only while a command line
	 ** is actually being read; otherwise, we'll end up bequeathing
	 ** atexit() handlers and other unwanted stuff to our
	 ** child processes (rob@sysgo.de)
	 */
	r = read_line_input(prompt_str, the_command, BUFSIZ, line_input_state);
#else
	fputs(prompt_str, stdout);
	fflush(stdout);
	the_command[0] = r = fgetc(i->file);
	the_command[1] = '\0';
#endif
	fflush(stdout);
	i->p = the_command;
	return r; /* < 0 == EOF. Not meaningful otherwise */
}

/* This is the magic location that prints prompts
 * and gets data back from the user */
static int file_get(struct in_str *i)
{
	int ch;

	ch = 0;
	/* If there is data waiting, eat it up */
	if (i->p && *i->p) {
		ch = *i->p++;
	} else {
		/* need to double check i->file because we might be doing something
		 * more complicated by now, like sourcing or substituting. */
		if (i->__promptme && interactive_fd && i->file == stdin) {
			while (!i->p || !(interactive_fd && i->p[0])) {
				if (get_user_input(i) < 0)
					return EOF;
			}
			i->promptmode = 2;
			i->__promptme = 0;
			if (i->p && *i->p) {
				ch = *i->p++;
			}
		} else {
			ch = fgetc(i->file);
		}

		debug_printf("b_getch: got a %d\n", ch);
	}
	if (ch == '\n')
		i->__promptme = 1;
	return ch;
}

/* All the callers guarantee this routine will never be
 * used right after a newline, so prompting is not needed.
 */
static int file_peek(struct in_str *i)
{
	if (i->p && *i->p) {
		return *i->p;
	}
	i->peek_buf[0] = fgetc(i->file);
	i->peek_buf[1] = '\0';
	i->p = i->peek_buf;
	debug_printf("b_peek: got a %d\n", *i->p);
	return *i->p;
}

static void setup_file_in_str(struct in_str *i, FILE *f)
{
	i->peek = file_peek;
	i->get = file_get;
	i->__promptme = 1;
	i->promptmode = 1;
	i->file = f;
	i->p = NULL;
}

static void setup_string_in_str(struct in_str *i, const char *s)
{
	i->peek = static_peek;
	i->get = static_get;
	i->__promptme = 1;
	i->promptmode = 1;
	i->p = s;
}

static void mark_open(int fd)
{
	struct close_me *new = xmalloc(sizeof(struct close_me));
	new->fd = fd;
	new->next = close_me_head;
	close_me_head = new;
}

static void mark_closed(int fd)
{
	struct close_me *tmp;
	if (close_me_head == NULL || close_me_head->fd != fd)
		bb_error_msg_and_die("corrupt close_me");
	tmp = close_me_head;
	close_me_head = close_me_head->next;
	free(tmp);
}

static void close_all(void)
{
	struct close_me *c;
	for (c = close_me_head; c; c = c->next) {
		close(c->fd);
	}
	close_me_head = NULL;
}

/* squirrel != NULL means we squirrel away copies of stdin, stdout,
 * and stderr if they are redirected. */
static int setup_redirects(struct child_prog *prog, int squirrel[])
{
	int openfd, mode;
	struct redir_struct *redir;

	for (redir = prog->redirects; redir; redir = redir->next) {
		if (redir->dup == -1 && redir->word.gl_pathv == NULL) {
			/* something went wrong in the parse.  Pretend it didn't happen */
			continue;
		}
		if (redir->dup == -1) {
			mode = redir_table[redir->type].mode;
			openfd = open_or_warn(redir->word.gl_pathv[0], mode);
			if (openfd < 0) {
			/* this could get lost if stderr has been redirected, but
			   bash and ash both lose it as well (though zsh doesn't!) */
				return 1;
			}
		} else {
			openfd = redir->dup;
		}

		if (openfd != redir->fd) {
			if (squirrel && redir->fd < 3) {
				squirrel[redir->fd] = dup(redir->fd);
			}
			if (openfd == -3) {
				close(openfd);
			} else {
				dup2(openfd, redir->fd);
				if (redir->dup == -1)
					close(openfd);
			}
		}
	}
	return 0;
}

static void restore_redirects(int squirrel[])
{
	int i, fd;
	for (i = 0; i < 3; i++) {
		fd = squirrel[i];
		if (fd != -1) {
			/* No error checking.  I sure wouldn't know what
			 * to do with an error if I found one! */
			dup2(fd, i);
			close(fd);
		}
	}
}

/* never returns */
/* XXX no exit() here.  If you don't exec, use _exit instead.
 * The at_exit handlers apparently confuse the calling process,
 * in particular stdin handling.  Not sure why? -- because of vfork! (vda) */
static void pseudo_exec_argv(char **argv)
{
	int i, rcode;
	char *p;
	const struct built_in_command *x;

	for (i = 0; is_assignment(argv[i]); i++) {
		debug_printf("pid %d environment modification: %s\n",
				getpid(), argv[i]);
	// FIXME: vfork case??
		p = insert_var_value(argv[i]);
		putenv(strdup(p));
		if (p != argv[i])
			free(p);
	}
	argv += i;
	/* If a variable is assigned in a forest, and nobody listens,
	 * was it ever really set?
	 */
	if (argv[0] == NULL) {
		_exit(EXIT_SUCCESS);
	}

	/*
	 * Check if the command matches any of the builtins.
	 * Depending on context, this might be redundant.  But it's
	 * easier to waste a few CPU cycles than it is to figure out
	 * if this is one of those cases.
	 */
	for (x = bltins; x->cmd; x++) {
		if (strcmp(argv[0], x->cmd) == 0) {
			debug_printf("builtin exec %s\n", argv[0]);
			rcode = x->function(argv);
			fflush(stdout);
			_exit(rcode);
		}
	}

	/* Check if the command matches any busybox internal commands
	 * ("applets") here.
	 * FIXME: This feature is not 100% safe, since
	 * BusyBox is not fully reentrant, so we have no guarantee the things
	 * from the .bss are still zeroed, or that things from .data are still
	 * at their defaults.  We could exec ourself from /proc/self/exe, but I
	 * really dislike relying on /proc for things.  We could exec ourself
	 * from global_argv[0], but if we are in a chroot, we may not be able
	 * to find ourself... */
#if ENABLE_FEATURE_SH_STANDALONE
	debug_printf("running applet %s\n", argv[0]);
	run_applet_and_exit(argv[0], argv);
// is it ok that run_applet_and_exit() does exit(), not _exit()?
// NB: IIRC on NOMMU we are after _vfork_, not fork!
#endif
	debug_printf("exec of %s\n", argv[0]);
	execvp(argv[0], argv);
	bb_perror_msg("cannot exec '%s'", argv[0]);
	_exit(1);
}

static void pseudo_exec(struct child_prog *child)
{
	int rcode;

	if (child->argv) {
		pseudo_exec_argv(child->argv);
	}

	if (child->group) {
		debug_printf("runtime nesting to group\n");
	// FIXME: do not modify globals! Think vfork!
#if ENABLE_HUSH_INTERACTIVE
		interactive_fd = 0;    /* crucial!!!! */
#endif
		rcode = run_list_real(child->group);
		/* OK to leak memory by not calling free_pipe_list,
		 * since this process is about to exit */
		_exit(rcode);
	}

	/* Can happen.  See what bash does with ">foo" by itself. */
	debug_printf("trying to pseudo_exec null command\n");
	_exit(EXIT_SUCCESS);
}

#if ENABLE_HUSH_INTERACTIVE
static const char *get_cmdtext(struct pipe *pi)
{
	char **argv;
	char *p;
	int len;

	/* This is subtle. ->cmdtext is created only on first backgrounding.
	 * (Think "cat, <ctrl-z>, fg, <ctrl-z>, fg, <ctrl-z>...." here...)
	 * On subsequent bg argv can be trashed, but we won't use it */
	if (pi->cmdtext)
		return pi->cmdtext;
	argv = pi->progs[0].argv;
	if (!argv || !argv[0])
		return (pi->cmdtext = xzalloc(1));

	len = 0;
	do len += strlen(*argv) + 1; while (*++argv);
	pi->cmdtext = p = xmalloc(len);
	argv = pi->progs[0].argv;
	do {
		len = strlen(*argv);
		memcpy(p, *argv, len);
		p += len;
		*p++ = ' ';
	} while (*++argv);
	p[-1] = '\0';
	return pi->cmdtext;
}
#endif

#if ENABLE_HUSH_INTERACTIVE
static void insert_bg_job(struct pipe *pi)
{
	struct pipe *thejob;

	/* Linear search for the ID of the job to use */
	pi->jobid = 1;
	for (thejob = job_list; thejob; thejob = thejob->next)
		if (thejob->jobid >= pi->jobid)
			pi->jobid = thejob->jobid + 1;

	/* add thejob to the list of running jobs */
	if (!job_list) {
		thejob = job_list = xmalloc(sizeof(*thejob));
	} else {
		for (thejob = job_list; thejob->next; thejob = thejob->next)
			continue;
		thejob->next = xmalloc(sizeof(*thejob));
		thejob = thejob->next;
	}

	/* physically copy the struct job */
	memcpy(thejob, pi, sizeof(struct pipe));
	thejob->progs = xmalloc(sizeof(pi->progs[0]) * pi->num_progs);
	memcpy(thejob->progs, pi->progs, sizeof(pi->progs[0]) * pi->num_progs);
	thejob->next = NULL;
	/*seems to be wrong:*/
	/*thejob->running_progs = thejob->num_progs;*/
	/*thejob->stopped_progs = 0;*/
	thejob->cmdtext = xstrdup(get_cmdtext(pi));

	/* we don't wait for background thejobs to return -- append it
	   to the list of backgrounded thejobs and leave it alone */
	printf("[%d] %d %s\n", thejob->jobid, thejob->progs[0].pid, thejob->cmdtext);
	last_bg_pid = thejob->progs[0].pid;
	last_jobid = thejob->jobid;
}

static void remove_bg_job(struct pipe *pi)
{
	struct pipe *prev_pipe;

	if (pi == job_list) {
		job_list = pi->next;
	} else {
		prev_pipe = job_list;
		while (prev_pipe->next != pi)
			prev_pipe = prev_pipe->next;
		prev_pipe->next = pi->next;
	}
	if (job_list)
		last_jobid = job_list->jobid;
	else
		last_jobid = 0;
}

/* remove a backgrounded job */
static void delete_finished_bg_job(struct pipe *pi)
{
	remove_bg_job(pi);
	pi->stopped_progs = 0;
	free_pipe(pi, 0);
	free(pi);
}
#endif

/* Checks to see if any processes have exited -- if they
   have, figure out why and see if a job has completed */
static int checkjobs(struct pipe* fg_pipe)
{
	int attributes;
	int status;
#if ENABLE_HUSH_INTERACTIVE
	int prognum = 0;
	struct pipe *pi;
#endif
	pid_t childpid;
	int rcode = 0;

	attributes = WUNTRACED;
	if (fg_pipe == NULL) {
		attributes |= WNOHANG;
	}

/* Do we do this right?
 * bash-3.00# sleep 20 | false
 * <Ctrl-Z pressed>
 * [3]+  Stopped          sleep 20 | false
 * bash-3.00# echo $?
 * 1   <========== bg pipe is not fully done, but exitcode is already known!
 */

//FIXME: non-interactive bash does not continue even if all processes in fg pipe
//are stopped. Testcase: "cat | cat" in a script (not on command line)
// + killall -STOP cat

 wait_more:
	while ((childpid = waitpid(-1, &status, attributes)) > 0) {
		const int dead = WIFEXITED(status) || WIFSIGNALED(status);

#ifdef DEBUG_SHELL_JOBS
		if (WIFSTOPPED(status))
			debug_jobs_printf("pid %d stopped by sig %d (exitcode %d)\n",
					childpid, WSTOPSIG(status), WEXITSTATUS(status));
		if (WIFSIGNALED(status))
			debug_jobs_printf("pid %d killed by sig %d (exitcode %d)\n",
					childpid, WTERMSIG(status), WEXITSTATUS(status));
		if (WIFEXITED(status))
			debug_jobs_printf("pid %d exited, exitcode %d\n",
					childpid, WEXITSTATUS(status));
#endif
		/* Were we asked to wait for fg pipe? */
		if (fg_pipe) {
			int i;
			for (i = 0; i < fg_pipe->num_progs; i++) {
				debug_jobs_printf("check pid %d\n", fg_pipe->progs[i].pid);
				if (fg_pipe->progs[i].pid == childpid) {
					/* printf("process %d exit %d\n", i, WEXITSTATUS(status)); */
					if (dead) {
						fg_pipe->progs[i].pid = 0;
						fg_pipe->running_progs--;
						if (i == fg_pipe->num_progs-1)
							/* last process gives overall exitstatus */
							rcode = WEXITSTATUS(status);
					} else {
						fg_pipe->progs[i].is_stopped = 1;
						fg_pipe->stopped_progs++;
					}
					debug_jobs_printf("fg_pipe: running_progs %d stopped_progs %d\n",
							fg_pipe->running_progs, fg_pipe->stopped_progs);
					if (fg_pipe->running_progs - fg_pipe->stopped_progs <= 0) {
						/* All processes in fg pipe have exited/stopped */
#if ENABLE_HUSH_INTERACTIVE
						if (fg_pipe->running_progs)
							insert_bg_job(fg_pipe);
#endif
						return rcode;
					}
					/* There are still running processes in the fg pipe */
					goto wait_more;
				}
			}
			/* fall through to searching process in bg pipes */
		}

#if ENABLE_HUSH_INTERACTIVE
		/* We asked to wait for bg or orphaned children */
		/* No need to remember exitcode in this case */
		for (pi = job_list; pi; pi = pi->next) {
			prognum = 0;
			while (prognum < pi->num_progs) {
				if (pi->progs[prognum].pid == childpid)
					goto found_pi_and_prognum;
				prognum++;
			}
		}
#endif

		/* Happens when shell is used as init process (init=/bin/sh) */
		debug_printf("checkjobs: pid %d was not in our list!\n", childpid);
		goto wait_more;

#if ENABLE_HUSH_INTERACTIVE
 found_pi_and_prognum:
		if (dead) {
			/* child exited */
			pi->progs[prognum].pid = 0;
			pi->running_progs--;
			if (!pi->running_progs) {
				printf(JOB_STATUS_FORMAT, pi->jobid,
							"Done", pi->cmdtext);
				delete_finished_bg_job(pi);
			}
		} else {
			/* child stopped */
			pi->stopped_progs++;
			pi->progs[prognum].is_stopped = 1;
		}
#endif
	}

	/* wait found no children or failed */

	if (childpid && errno != ECHILD)
		bb_perror_msg("waitpid");

	/* move the shell to the foreground */
	//if (interactive_fd && tcsetpgrp(interactive_fd, getpgid(0)))
	//	bb_perror_msg("tcsetpgrp-2");
	return rcode;
}

#if ENABLE_HUSH_INTERACTIVE
static int checkjobs_and_fg_shell(struct pipe* fg_pipe)
{
	pid_t p;
	int rcode = checkjobs(fg_pipe);
	/* Job finished, move the shell to the foreground */
	p = getpgid(0);
	debug_printf("fg'ing ourself: getpgid(0)=%d\n", (int)p);
	if (tcsetpgrp(interactive_fd, p) && errno != ENOTTY)
		bb_perror_msg("tcsetpgrp-4a");
	return rcode;
}
#endif

#if ENABLE_FEATURE_SH_STANDALONE
/* run_pipe_real's helper */
static int run_single_fg_nofork(struct pipe *pi, const struct bb_applet *a,
		char **argv)
{
#if ENABLE_HUSH_INTERACTIVE
	int rcode;
	/* TSTP handler will store pid etc in pi */
	nofork_pipe = pi;
	save_nofork_data(&nofork_save);
	if (sigsetjmp(nofork_jb, 1) == 0) {
		signal_SA_RESTART(SIGTSTP, handler_ctrl_z);
		signal(SIGINT, handler_ctrl_c);
		rcode = run_nofork_applet_prime(&nofork_save, a, argv);
		if (--nofork_save.saved != 0) {
			/* Ctrl-Z forked, we are child */
			exit(rcode);
		}
		return rcode;
	}
	/* Ctrl-Z forked, we are parent; or Ctrl-C.
	 * Sighandler has longjmped us here */
	signal(SIGINT, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	debug_jobs_printf("Exiting nofork early\n");
	restore_nofork_data(&nofork_save);
	if (nofork_save.saved == 0) /* Ctrl-Z, not Ctrl-C */
		insert_bg_job(pi);
	else
		putchar('\n'); /* bash does this on Ctrl-C */
	return 0;
#else
	return run_nofork_applet(a, argv);
#endif
}
#endif

/* run_pipe_real() starts all the jobs, but doesn't wait for anything
 * to finish.  See checkjobs().
 *
 * return code is normally -1, when the caller has to wait for children
 * to finish to determine the exit status of the pipe.  If the pipe
 * is a simple builtin command, however, the action is done by the
 * time run_pipe_real returns, and the exit code is provided as the
 * return value.
 *
 * The input of the pipe is always stdin, the output is always
 * stdout.  The outpipe[] mechanism in BusyBox-0.48 lash is bogus,
 * because it tries to avoid running the command substitution in
 * subshell, when that is in fact necessary.  The subshell process
 * now has its stdout directed to the input of the appropriate pipe,
 * so this routine is noticeably simpler.
 */
static int run_pipe_real(struct pipe *pi)
{
	int i;
	int nextin, nextout;
	int pipefds[2];				/* pipefds[0] is for reading */
	struct child_prog *child;
	const struct built_in_command *x;
	char *p;
	/* it is not always needed, but we aim to smaller code */
	int squirrel[] = { -1, -1, -1 };
	int rcode;
	const int single_fg = (pi->num_progs == 1 && pi->followup != PIPE_BG);

	nextin = 0;
#if ENABLE_HUSH_INTERACTIVE
	pi->pgrp = -1;
#endif
	pi->running_progs = 0;
	pi->stopped_progs = 0;

	/* Check if this is a simple builtin (not part of a pipe).
	 * Builtins within pipes have to fork anyway, and are handled in
	 * pseudo_exec.  "echo foo | read bar" doesn't work on bash, either.
	 */
	child = &(pi->progs[0]);
	if (single_fg && child->group && child->subshell == 0) {
		debug_printf("non-subshell grouping\n");
		setup_redirects(child, squirrel);
		/* XXX could we merge code with following builtin case,
		 * by creating a pseudo builtin that calls run_list_real? */
		rcode = run_list_real(child->group);
		restore_redirects(squirrel);
		return rcode;
	}

	if (single_fg && child->argv != NULL) {
		char **argv = child->argv;

		for (i = 0; is_assignment(argv[i]); i++)
			continue;
		if (i != 0 && argv[i] == NULL) {
			/* assignments, but no command: set the local environment */
			for (i = 0; argv[i] != NULL; i++) {
				/* Ok, this case is tricky.  We have to decide if this is a
				 * local variable, or an already exported variable.  If it is
				 * already exported, we have to export the new value.  If it is
				 * not exported, we need only set this as a local variable.
				 * This junk is all to decide whether or not to export this
				 * variable. */
				int export_me = 0;
				char *name, *value;
				name = xstrdup(argv[i]);
				debug_printf("Local environment set: %s\n", name);
				value = strchr(name, '=');
				if (value)
					*value = 0;
				if (get_local_var(name)) {
					export_me = 1;
				}
				free(name);
				p = insert_var_value(argv[i]);
				set_local_var(p, export_me);
				if (p != argv[i])
					free(p);
			}
			return EXIT_SUCCESS;   /* don't worry about errors in set_local_var() yet */
		}
		for (i = 0; is_assignment(argv[i]); i++) {
			p = insert_var_value(argv[i]);
			putenv(strdup(p));
			if (p != argv[i]) {
				child->sp--;
				free(p);
			}
		}
		if (child->sp) {
			char *str;

			str = make_string(argv + i);
			parse_string_outer(str, FLAG_EXIT_FROM_LOOP | FLAG_REPARSING);
			free(str);
			return last_return_code;
		}
		for (x = bltins; x->cmd; x++) {
			if (strcmp(argv[i], x->cmd) == 0) {
				if (x->function == builtin_exec && argv[i+1] == NULL) {
					debug_printf("magic exec\n");
					setup_redirects(child, NULL);
					return EXIT_SUCCESS;
				}
				debug_printf("builtin inline %s\n", argv[0]);
				/* XXX setup_redirects acts on file descriptors, not FILEs.
				 * This is perfect for work that comes after exec().
				 * Is it really safe for inline use?  Experimentally,
				 * things seem to work with glibc. */
// TODO: fflush(NULL)?
				setup_redirects(child, squirrel);
				rcode = x->function(argv + i);
				restore_redirects(squirrel);
				return rcode;
			}
		}
#if ENABLE_FEATURE_SH_STANDALONE
		{
			const struct bb_applet *a = find_applet_by_name(argv[i]);
			if (a && a->nofork) {
				setup_redirects(child, squirrel);
				rcode = run_single_fg_nofork(pi, a, argv + i);
				restore_redirects(squirrel);
				return rcode;
			}
		}
#endif
	}

	/* Going to fork a child per each pipe member */

	/* Disable job control signals for shell (parent) and
	 * for initial child code after fork */
	set_jobctrl_sighandler(SIG_IGN);

	for (i = 0; i < pi->num_progs; i++) {
		child = &(pi->progs[i]);

		/* pipes are inserted between pairs of commands */
		if ((i + 1) < pi->num_progs) {
			if (pipe(pipefds) < 0)
				bb_perror_msg_and_die("pipe");
			nextout = pipefds[1];
		} else {
			nextout = 1;
			pipefds[0] = -1;
		}

		/* XXX test for failed fork()? */
#if BB_MMU
		child->pid = fork();
#else
		child->pid = vfork();
#endif
		if (!child->pid) { /* child */
			/* Every child adds itself to new process group
			 * with pgid == pid of first child in pipe */
#if ENABLE_HUSH_INTERACTIVE
			if (interactive_fd) {
				/* Don't do pgrp restore anymore on fatal signals */
				set_fatal_sighandler(SIG_DFL);
				if (pi->pgrp < 0) /* true for 1st process only */
					pi->pgrp = getpid();
				if (setpgid(0, pi->pgrp) == 0 && pi->followup != PIPE_BG) {
					/* We do it in *every* child, not just first,
					 * to avoid races */
					tcsetpgrp(interactive_fd, pi->pgrp);
				}
			}
#endif
			// in non-interactive case fatal sigs are already SIG_DFL
			close_all();
			if (nextin != 0) {
				dup2(nextin, 0);
				close(nextin);
			}
			if (nextout != 1) {
				dup2(nextout, 1);
				close(nextout);
			}
			if (pipefds[0] != -1) {
				close(pipefds[0]);  /* opposite end of our output pipe */
			}
			/* Like bash, explicit redirects override pipes,
			 * and the pipe fd is available for dup'ing. */
			setup_redirects(child, NULL);

			/* Restore default handlers just prior to exec */
			set_jobctrl_sighandler(SIG_DFL);
			set_misc_sighandler(SIG_DFL);
			signal(SIGCHLD, SIG_DFL);
			pseudo_exec(child);
		}

		pi->running_progs++;

#if ENABLE_HUSH_INTERACTIVE
		/* Second and next children need to know pid of first one */
		if (pi->pgrp < 0)
			pi->pgrp = child->pid;
#endif

		/* Don't check for errors.  The child may be dead already,
		 * in which case setpgid returns error code EACCES. */
		//why we do it at all?? child does it itself
		//if (interactive_fd)
		//	setpgid(child->pid, pi->pgrp);

		if (nextin != 0)
			close(nextin);
		if (nextout != 1)
			close(nextout);

		/* If there isn't another process, nextin is garbage
		   but it doesn't matter */
		nextin = pipefds[0];
	}
	return -1;
}

static int run_list_real(struct pipe *pi)
{
	char *save_name = NULL;
	char **list = NULL;
	char **save_list = NULL;
	struct pipe *rpipe;
	int flag_rep = 0;
	int save_num_progs;
	int rcode = 0, flag_skip = 1;
	int flag_restore = 0;
	int if_code = 0, next_if_code = 0;  /* need double-buffer to handle elif */
	reserved_style rmode, skip_more_in_this_rmode = RES_XXXX;
	/* check syntax for "for" */
	for (rpipe = pi; rpipe; rpipe = rpipe->next) {
		if ((rpipe->r_mode == RES_IN || rpipe->r_mode == RES_FOR)
		 && (rpipe->next == NULL)
		) {
			syntax();
			return 1;
		}
		if ((rpipe->r_mode == RES_IN &&	rpipe->next->r_mode == RES_IN && rpipe->next->progs->argv != NULL)
		 || (rpipe->r_mode == RES_FOR && rpipe->next->r_mode != RES_IN)
		) {
			syntax();
			return 1;
		}
	}
	for (; pi; pi = (flag_restore != 0) ? rpipe : pi->next) {
		if (pi->r_mode == RES_WHILE || pi->r_mode == RES_UNTIL
		 || pi->r_mode == RES_FOR
		) {
			flag_restore = 0;
			if (!rpipe) {
				flag_rep = 0;
				rpipe = pi;
			}
		}
		rmode = pi->r_mode;
		debug_printf("rmode=%d  if_code=%d  next_if_code=%d skip_more=%d\n",
				rmode, if_code, next_if_code, skip_more_in_this_rmode);
		if (rmode == skip_more_in_this_rmode && flag_skip) {
			if (pi->followup == PIPE_SEQ)
				flag_skip = 0;
			continue;
		}
		flag_skip = 1;
		skip_more_in_this_rmode = RES_XXXX;
		if (rmode == RES_THEN || rmode == RES_ELSE)
			if_code = next_if_code;
		if (rmode == RES_THEN && if_code)
			continue;
		if (rmode == RES_ELSE && !if_code)
			continue;
		if (rmode == RES_ELIF && !if_code)
			break;
		if (rmode == RES_FOR && pi->num_progs) {
			if (!list) {
				/* if no variable values after "in" we skip "for" */
				if (!pi->next->progs->argv)
					continue;
				/* create list of variable values */
				list = make_list_in(pi->next->progs->argv,
						pi->progs->argv[0]);
				save_list = list;
				save_name = pi->progs->argv[0];
				pi->progs->argv[0] = NULL;
				flag_rep = 1;
			}
			if (!*list) {
				free(pi->progs->argv[0]);
				free(save_list);
				list = NULL;
				flag_rep = 0;
				pi->progs->argv[0] = save_name;
				pi->progs->glob_result.gl_pathv[0] = pi->progs->argv[0];
				continue;
			}
			/* insert new value from list for variable */
			if (pi->progs->argv[0])
				free(pi->progs->argv[0]);
			pi->progs->argv[0] = *list++;
			pi->progs->glob_result.gl_pathv[0] = pi->progs->argv[0];
		}
		if (rmode == RES_IN)
			continue;
		if (rmode == RES_DO) {
			if (!flag_rep)
				continue;
		}
		if (rmode == RES_DONE) {
			if (flag_rep) {
				flag_restore = 1;
			} else {
				rpipe = NULL;
			}
		}
		if (pi->num_progs == 0)
			continue;
		save_num_progs = pi->num_progs; /* save number of programs */
		rcode = run_pipe_real(pi);
		debug_printf("run_pipe_real returned %d\n", rcode);
		if (rcode != -1) {
			/* We only ran a builtin: rcode was set by the return value
			 * of run_pipe_real(), and we don't need to wait for anything. */
		} else if (pi->followup == PIPE_BG) {
			/* XXX check bash's behavior with nontrivial pipes */
			/* XXX compute jobid */
			/* XXX what does bash do with attempts to background builtins? */
#if ENABLE_HUSH_INTERACTIVE
			insert_bg_job(pi);
#endif
			rcode = EXIT_SUCCESS;
		} else {
			if (interactive_fd) {
				rcode = checkjobs_and_fg_shell(pi);
			} else {
				rcode = checkjobs(pi);
			}
			debug_printf("checkjobs returned %d\n", rcode);
		}
		last_return_code = rcode;
		pi->num_progs = save_num_progs; /* restore number of programs */
		if (rmode == RES_IF || rmode == RES_ELIF)
			next_if_code = rcode;  /* can be overwritten a number of times */
		if (rmode == RES_WHILE)
			flag_rep = !last_return_code;
		if (rmode == RES_UNTIL)
			flag_rep = last_return_code;
		if ((rcode == EXIT_SUCCESS && pi->followup == PIPE_OR)
		 || (rcode != EXIT_SUCCESS && pi->followup == PIPE_AND)
		) {
			skip_more_in_this_rmode = rmode;
		}
		checkjobs(NULL);
	}
	return rcode;
}

/* return code is the exit status of the pipe */
static int free_pipe(struct pipe *pi, int indent)
{
	char **p;
	struct child_prog *child;
	struct redir_struct *r, *rnext;
	int a, i, ret_code = 0;

	if (pi->stopped_progs > 0)
		return ret_code;
	final_printf("%s run pipe: (pid %d)\n", indenter(indent), getpid());
	for (i = 0; i < pi->num_progs; i++) {
		child = &pi->progs[i];
		final_printf("%s  command %d:\n", indenter(indent), i);
		if (child->argv) {
			for (a = 0, p = child->argv; *p; a++, p++) {
				final_printf("%s   argv[%d] = %s\n", indenter(indent), a, *p);
			}
			globfree(&child->glob_result);
			child->argv = NULL;
		} else if (child->group) {
			final_printf("%s   begin group (subshell:%d)\n", indenter(indent), child->subshell);
			ret_code = free_pipe_list(child->group, indent+3);
			final_printf("%s   end group\n", indenter(indent));
		} else {
			final_printf("%s   (nil)\n", indenter(indent));
		}
		for (r = child->redirects; r; r = rnext) {
			final_printf("%s   redirect %d%s", indenter(indent), r->fd, redir_table[r->type].descrip);
			if (r->dup == -1) {
				/* guard against the case >$FOO, where foo is unset or blank */
				if (r->word.gl_pathv) {
					final_printf(" %s\n", *r->word.gl_pathv);
					globfree(&r->word);
				}
			} else {
				final_printf("&%d\n", r->dup);
			}
			rnext = r->next;
			free(r);
		}
		child->redirects = NULL;
	}
	free(pi->progs);   /* children are an array, they get freed all at once */
	pi->progs = NULL;
#if ENABLE_HUSH_INTERACTIVE
	free(pi->cmdtext);
	pi->cmdtext = NULL;
#endif
	return ret_code;
}

static int free_pipe_list(struct pipe *head, int indent)
{
	int rcode = 0;   /* if list has no members */
	struct pipe *pi, *next;
	for (pi = head; pi; pi = next) {
		final_printf("%s pipe reserved mode %d\n", indenter(indent), pi->r_mode);
		rcode = free_pipe(pi, indent);
		final_printf("%s pipe followup code %d\n", indenter(indent), pi->followup);
		next = pi->next;
		/*pi->next = NULL;*/
		free(pi);
	}
	return rcode;
}

/* Select which version we will use */
static int run_list(struct pipe *pi)
{
	int rcode = 0;
	if (fake_mode == 0) {
		rcode = run_list_real(pi);
	}
	/* free_pipe_list has the side effect of clearing memory
	 * In the long run that function can be merged with run_list_real,
	 * but doing that now would hobble the debugging effort. */
	free_pipe_list(pi,0);
	return rcode;
}

/* The API for glob is arguably broken.  This routine pushes a non-matching
 * string into the output structure, removing non-backslashed backslashes.
 * If someone can prove me wrong, by performing this function within the
 * original glob(3) api, feel free to rewrite this routine into oblivion.
 * Return code (0 vs. GLOB_NOSPACE) matches glob(3).
 * XXX broken if the last character is '\\', check that before calling.
 */
static int globhack(const char *src, int flags, glob_t *pglob)
{
	int cnt = 0, pathc;
	const char *s;
	char *dest;
	for (cnt = 1, s = src; s && *s; s++) {
		if (*s == '\\') s++;
		cnt++;
	}
	dest = malloc(cnt);
	if (!dest)
		return GLOB_NOSPACE;
	if (!(flags & GLOB_APPEND)) {
		pglob->gl_pathv = NULL;
		pglob->gl_pathc = 0;
		pglob->gl_offs = 0;
		pglob->gl_offs = 0;
	}
	pathc = ++pglob->gl_pathc;
	pglob->gl_pathv = realloc(pglob->gl_pathv, (pathc+1)*sizeof(*pglob->gl_pathv));
	if (pglob->gl_pathv == NULL)
		return GLOB_NOSPACE;
	pglob->gl_pathv[pathc-1] = dest;
	pglob->gl_pathv[pathc] = NULL;
	for (s = src; s && *s; s++, dest++) {
		if (*s == '\\') s++;
		*dest = *s;
	}
	*dest = '\0';
	return 0;
}

/* XXX broken if the last character is '\\', check that before calling */
static int glob_needed(const char *s)
{
	for (; *s; s++) {
		if (*s == '\\') s++;
		if (strchr("*[?", *s)) return 1;
	}
	return 0;
}

static int xglob(o_string *dest, int flags, glob_t *pglob)
{
	int gr;

	/* short-circuit for null word */
	/* we can code this better when the debug_printf's are gone */
	if (dest->length == 0) {
		if (dest->nonnull) {
			/* bash man page calls this an "explicit" null */
			gr = globhack(dest->data, flags, pglob);
			debug_printf("globhack returned %d\n", gr);
		} else {
			return 0;
		}
	} else if (glob_needed(dest->data)) {
		gr = glob(dest->data, flags, NULL, pglob);
		debug_printf("glob returned %d\n", gr);
		if (gr == GLOB_NOMATCH) {
			/* quote removal, or more accurately, backslash removal */
			gr = globhack(dest->data, flags, pglob);
			debug_printf("globhack returned %d\n", gr);
		}
	} else {
		gr = globhack(dest->data, flags, pglob);
		debug_printf("globhack returned %d\n", gr);
	}
	if (gr == GLOB_NOSPACE)
		bb_error_msg_and_die("out of memory during glob");
	if (gr != 0) { /* GLOB_ABORTED ? */
		bb_error_msg("glob(3) error %d", gr);
	}
	/* globprint(glob_target); */
	return gr;
}

static char **make_list_in(char **inp, char *name)
{
	int len, i;
	int name_len = strlen(name);
	int n = 0;
	char **list;
	char *p1, *p2, *p3;

	/* create list of variable values */
	list = xmalloc(sizeof(*list));
	for (i = 0; inp[i]; i++) {
		p3 = insert_var_value(inp[i]);
		p1 = p3;
		while (*p1) {
			if ((*p1 == ' ')) {
				p1++;
				continue;
			}
			p2 = strchr(p1, ' ');
			if (p2) {
				len = p2 - p1;
			} else {
				len = strlen(p1);
				p2 = p1 + len;
			}
			/* we use n + 2 in realloc for list, because we add
			 * new element and then we will add NULL element */
			list = xrealloc(list, sizeof(*list) * (n + 2));
			list[n] = xmalloc(2 + name_len + len);
			strcpy(list[n], name);
			strcat(list[n], "=");
			strncat(list[n], p1, len);
			list[n++][name_len + len + 1] = '\0';
			p1 = p2;
		}
		if (p3 != inp[i]) free(p3);
	}
	list[n] = NULL;
	return list;
}

static char *insert_var_value(char *inp)
{
	int res_str_len = 0;
	int len;
	int done = 0;
	char *p, *res_str = NULL;
	const char *p1;

	while ((p = strchr(inp, SPECIAL_VAR_SYMBOL))) {
		if (p != inp) {
			len = p - inp;
			res_str = xrealloc(res_str, (res_str_len + len));
			strncpy((res_str + res_str_len), inp, len);
			res_str_len += len;
		}
		inp = ++p;
		p = strchr(inp, SPECIAL_VAR_SYMBOL);
		*p = '\0';
		p1 = lookup_param(inp);
		if (p1) {
			len = res_str_len + strlen(p1);
			res_str = xrealloc(res_str, (1 + len));
			strcpy((res_str + res_str_len), p1);
			res_str_len = len;
		}
		*p = SPECIAL_VAR_SYMBOL;
		inp = ++p;
		done = 1;
	}
	if (done) {
		res_str = xrealloc(res_str, (1 + res_str_len + strlen(inp)));
		strcpy((res_str + res_str_len), inp);
		while ((p = strchr(res_str, '\n'))) {
			*p = ' ';
		}
	}
	return (res_str == NULL) ? inp : res_str;
}

/* This is used to get/check local shell variables */
static const char *get_local_var(const char *s)
{
	struct variables *cur;

	if (!s)
		return NULL;
	for (cur = top_vars; cur; cur = cur->next)
		if (strcmp(cur->name, s) == 0)
			return cur->value;
	return NULL;
}

/* This is used to set local shell variables
   flg_export == 0 if only local (not exporting) variable
   flg_export == 1 if "new" exporting environ
   flg_export > 1  if current startup environ (not call putenv()) */
static int set_local_var(const char *s, int flg_export)
{
	char *name, *value;
	int result = 0;
	struct variables *cur;

	name = strdup(s);

	/* Assume when we enter this function that we are already in
	 * NAME=VALUE format.  So the first order of business is to
	 * split 's' on the '=' into 'name' and 'value' */
	value = strchr(name, '=');
	/*if (value == 0 && ++value == 0) ??? -vda */
	if (value == NULL || value[1] == '\0') {
		free(name);
		return -1;
	}
	*value++ = '\0';

	for (cur = top_vars; cur; cur = cur->next) {
		if (strcmp(cur->name, name) == 0)
			break;
	}

	if (cur) {
		if (strcmp(cur->value, value) == 0) {
			if (flg_export > 0 && cur->flg_export == 0)
				cur->flg_export = flg_export;
			else
				result++;
		} else if (cur->flg_read_only) {
			bb_error_msg("%s: readonly variable", name);
			result = -1;
		} else {
			if (flg_export > 0 || cur->flg_export > 1)
				cur->flg_export = 1;
			free((char*)cur->value);

			cur->value = strdup(value);
		}
	} else {
		cur = malloc(sizeof(struct variables));
		if (!cur) {
			result = -1;
		} else {
			cur->name = strdup(name);
			if (cur->name) {
				free(cur);
				result = -1;
			} else {
				struct variables *bottom = top_vars;
				cur->value = strdup(value);
				cur->next = 0;
				cur->flg_export = flg_export;
				cur->flg_read_only = 0;
				while (bottom->next)
					bottom = bottom->next;
				bottom->next = cur;
			}
		}
	}

	if (result == 0 && cur->flg_export == 1) {
		*(value-1) = '=';
		result = putenv(name);
	} else {
		free(name);
		if (result > 0)            /* equivalent to previous set */
			result = 0;
	}
	return result;
}

static void unset_local_var(const char *name)
{
	struct variables *cur, *next;

	if (!name)
		return;
	for (cur = top_vars; cur; cur = cur->next) {
		if (strcmp(cur->name, name) == 0) {
			if (cur->flg_read_only) {
				bb_error_msg("%s: readonly variable", name);
				return;
			}
			if (cur->flg_export)
				unsetenv(cur->name);
			free((char*)cur->name);
			free((char*)cur->value);
			next = top_vars;
			while (next->next != cur)
				next = next->next;
			next->next = cur->next;
			free(cur);
			return;
		}
	}
}

static int is_assignment(const char *s)
{
	if (!s || !isalpha(*s))
		return 0;
	s++;
	while (isalnum(*s) || *s == '_')
		s++;
	return *s == '=';
}

/* the src parameter allows us to peek forward to a possible &n syntax
 * for file descriptor duplication, e.g., "2>&1".
 * Return code is 0 normally, 1 if a syntax error is detected in src.
 * Resource errors (in xmalloc) cause the process to exit */
static int setup_redirect(struct p_context *ctx, int fd, redir_type style,
	struct in_str *input)
{
	struct child_prog *child = ctx->child;
	struct redir_struct *redir = child->redirects;
	struct redir_struct *last_redir = NULL;

	/* Create a new redir_struct and drop it onto the end of the linked list */
	while (redir) {
		last_redir = redir;
		redir = redir->next;
	}
	redir = xmalloc(sizeof(struct redir_struct));
	redir->next = NULL;
	redir->word.gl_pathv = NULL;
	if (last_redir) {
		last_redir->next = redir;
	} else {
		child->redirects = redir;
	}

	redir->type = style;
	redir->fd = (fd == -1) ? redir_table[style].default_fd : fd;

	debug_printf("Redirect type %d%s\n", redir->fd, redir_table[style].descrip);

	/* Check for a '2>&1' type redirect */
	redir->dup = redirect_dup_num(input);
	if (redir->dup == -2) return 1;  /* syntax error */
	if (redir->dup != -1) {
		/* Erik had a check here that the file descriptor in question
		 * is legit; I postpone that to "run time"
		 * A "-" representation of "close me" shows up as a -3 here */
		debug_printf("Duplicating redirect '%d>&%d'\n", redir->fd, redir->dup);
	} else {
		/* We do _not_ try to open the file that src points to,
		 * since we need to return and let src be expanded first.
		 * Set ctx->pending_redirect, so we know what to do at the
		 * end of the next parsed word.
		 */
		ctx->pending_redirect = redir;
	}
	return 0;
}

static struct pipe *new_pipe(void)
{
	struct pipe *pi;
	pi = xzalloc(sizeof(struct pipe));
	/*pi->num_progs = 0;*/
	/*pi->progs = NULL;*/
	/*pi->next = NULL;*/
	/*pi->followup = 0;  invalid */
	if (RES_NONE)
		pi->r_mode = RES_NONE;
	return pi;
}

static void initialize_context(struct p_context *ctx)
{
	ctx->pipe = NULL;
	ctx->pending_redirect = NULL;
	ctx->child = NULL;
	ctx->list_head = new_pipe();
	ctx->pipe = ctx->list_head;
	ctx->w = RES_NONE;
	ctx->stack = NULL;
	ctx->old_flag = 0;
	done_command(ctx);   /* creates the memory for working child */
}

/* normal return is 0
 * if a reserved word is found, and processed, return 1
 * should handle if, then, elif, else, fi, for, while, until, do, done.
 * case, function, and select are obnoxious, save those for later.
 */
static int reserved_word(o_string *dest, struct p_context *ctx)
{
	struct reserved_combo {
		char literal[7];
		unsigned char code;
		int flag;
	};
	/* Mostly a list of accepted follow-up reserved words.
	 * FLAG_END means we are done with the sequence, and are ready
	 * to turn the compound list into a command.
	 * FLAG_START means the word must start a new compound list.
	 */
	static const struct reserved_combo reserved_list[] = {
		{ "if",    RES_IF,    FLAG_THEN | FLAG_START },
		{ "then",  RES_THEN,  FLAG_ELIF | FLAG_ELSE | FLAG_FI },
		{ "elif",  RES_ELIF,  FLAG_THEN },
		{ "else",  RES_ELSE,  FLAG_FI   },
		{ "fi",    RES_FI,    FLAG_END  },
		{ "for",   RES_FOR,   FLAG_IN   | FLAG_START },
		{ "while", RES_WHILE, FLAG_DO   | FLAG_START },
		{ "until", RES_UNTIL, FLAG_DO   | FLAG_START },
		{ "in",    RES_IN,    FLAG_DO   },
		{ "do",    RES_DO,    FLAG_DONE },
		{ "done",  RES_DONE,  FLAG_END  }
	};
	enum { NRES = sizeof(reserved_list)/sizeof(reserved_list[0]) };
	const struct reserved_combo *r;

	for (r = reserved_list;	r < reserved_list+NRES; r++) {
		if (strcmp(dest->data, r->literal) == 0) {
			debug_printf("found reserved word %s, code %d\n", r->literal, r->code);
			if (r->flag & FLAG_START) {
				struct p_context *new = xmalloc(sizeof(struct p_context));
				debug_printf("push stack\n");
				if (ctx->w == RES_IN || ctx->w == RES_FOR) {
					syntax();
					free(new);
					ctx->w = RES_SNTX;
					b_reset(dest);
					return 1;
				}
				*new = *ctx;   /* physical copy */
				initialize_context(ctx);
				ctx->stack = new;
			} else if (ctx->w == RES_NONE || !(ctx->old_flag & (1 << r->code))) {
				syntax();
				ctx->w = RES_SNTX;
				b_reset(dest);
				return 1;
			}
			ctx->w = r->code;
			ctx->old_flag = r->flag;
			if (ctx->old_flag & FLAG_END) {
				struct p_context *old;
				debug_printf("pop stack\n");
				done_pipe(ctx, PIPE_SEQ);
				old = ctx->stack;
				old->child->group = ctx->list_head;
				old->child->subshell = 0;
				*ctx = *old;   /* physical copy */
				free(old);
			}
			b_reset(dest);
			return 1;
		}
	}
	return 0;
}

/* normal return is 0.
 * Syntax or xglob errors return 1. */
static int done_word(o_string *dest, struct p_context *ctx)
{
	struct child_prog *child = ctx->child;
	glob_t *glob_target;
	int gr, flags = 0;

	debug_printf("done_word: %s %p\n", dest->data, child);
	if (dest->length == 0 && !dest->nonnull) {
		debug_printf("  true null, ignored\n");
		return 0;
	}
	if (ctx->pending_redirect) {
		glob_target = &ctx->pending_redirect->word;
	} else {
		if (child->group) {
			syntax();
			return 1;  /* syntax error, groups and arglists don't mix */
		}
		if (!child->argv && (ctx->type & FLAG_PARSE_SEMICOLON)) {
			debug_printf("checking %s for reserved-ness\n", dest->data);
			if (reserved_word(dest, ctx))
				return (ctx->w == RES_SNTX);
		}
		glob_target = &child->glob_result;
		if (child->argv) flags |= GLOB_APPEND;
	}
	gr = xglob(dest, flags, glob_target);
	if (gr != 0) return 1;

	b_reset(dest);
	if (ctx->pending_redirect) {
		ctx->pending_redirect = NULL;
		if (glob_target->gl_pathc != 1) {
			bb_error_msg("ambiguous redirect");
			return 1;
		}
	} else {
		child->argv = glob_target->gl_pathv;
	}
	if (ctx->w == RES_FOR) {
		done_word(dest, ctx);
		done_pipe(ctx, PIPE_SEQ);
	}
	return 0;
}

/* The only possible error here is out of memory, in which case
 * xmalloc exits. */
static int done_command(struct p_context *ctx)
{
	/* The child is really already in the pipe structure, so
	 * advance the pipe counter and make a new, null child.
	 * Only real trickiness here is that the uncommitted
	 * child structure, to which ctx->child points, is not
	 * counted in pi->num_progs. */
	struct pipe *pi = ctx->pipe;
	struct child_prog *prog = ctx->child;

	if (prog && prog->group == NULL
	 && prog->argv == NULL
	 && prog->redirects == NULL
	) {
		debug_printf("done_command: skipping null command\n");
		return 0;
	}
	if (prog) {
		pi->num_progs++;
		debug_printf("done_command: num_progs incremented to %d\n", pi->num_progs);
	} else {
		debug_printf("done_command: initializing\n");
	}
	pi->progs = xrealloc(pi->progs, sizeof(*pi->progs) * (pi->num_progs+1));

	prog = pi->progs + pi->num_progs;
	memset(prog, 0, sizeof(*prog));
	/*prog->redirects = NULL;*/
	/*prog->argv = NULL; */
	/*prog->is_stopped = 0;*/
	/*prog->group = NULL;*/
	/*prog->glob_result.gl_pathv = NULL;*/
	prog->family = pi;
	/*prog->sp = 0;*/
	ctx->child = prog;
	prog->type = ctx->type;

	/* but ctx->pipe and ctx->list_head remain unchanged */
	return 0;
}

static int done_pipe(struct p_context *ctx, pipe_style type)
{
	struct pipe *new_p;
	done_command(ctx);  /* implicit closure of previous command */
	debug_printf("done_pipe, type %d\n", type);
	ctx->pipe->followup = type;
	ctx->pipe->r_mode = ctx->w;
	new_p = new_pipe();
	ctx->pipe->next = new_p;
	ctx->pipe = new_p;
	ctx->child = NULL;
	done_command(ctx);  /* set up new pipe to accept commands */
	return 0;
}

/* peek ahead in the in_str to find out if we have a "&n" construct,
 * as in "2>&1", that represents duplicating a file descriptor.
 * returns either -2 (syntax error), -1 (no &), or the number found.
 */
static int redirect_dup_num(struct in_str *input)
{
	int ch, d = 0, ok = 0;
	ch = b_peek(input);
	if (ch != '&') return -1;

	b_getch(input);  /* get the & */
	ch = b_peek(input);
	if (ch == '-') {
		b_getch(input);
		return -3;  /* "-" represents "close me" */
	}
	while (isdigit(ch)) {
		d = d*10 + (ch-'0');
		ok = 1;
		b_getch(input);
		ch = b_peek(input);
	}
	if (ok) return d;

	bb_error_msg("ambiguous redirect");
	return -2;
}

/* If a redirect is immediately preceded by a number, that number is
 * supposed to tell which file descriptor to redirect.  This routine
 * looks for such preceding numbers.  In an ideal world this routine
 * needs to handle all the following classes of redirects...
 *     echo 2>foo     # redirects fd  2 to file "foo", nothing passed to echo
 *     echo 49>foo    # redirects fd 49 to file "foo", nothing passed to echo
 *     echo -2>foo    # redirects fd  1 to file "foo",    "-2" passed to echo
 *     echo 49x>foo   # redirects fd  1 to file "foo",   "49x" passed to echo
 * A -1 output from this program means no valid number was found, so the
 * caller should use the appropriate default for this redirection.
 */
static int redirect_opt_num(o_string *o)
{
	int num;

	if (o->length == 0)
		return -1;
	for (num = 0; num < o->length; num++) {
		if (!isdigit(*(o->data + num))) {
			return -1;
		}
	}
	/* reuse num (and save an int) */
	num = atoi(o->data);
	b_reset(o);
	return num;
}

static FILE *generate_stream_from_list(struct pipe *head)
{
	FILE *pf;
	int pid, channel[2];
	if (pipe(channel) < 0) bb_perror_msg_and_die("pipe");
#if BB_MMU
	pid = fork();
#else
	pid = vfork();
#endif
	if (pid < 0) {
		bb_perror_msg_and_die("fork");
	} else if (pid == 0) {
		close(channel[0]);
		if (channel[1] != 1) {
			dup2(channel[1], 1);
			close(channel[1]);
		}
		_exit(run_list_real(head));   /* leaks memory */
	}
	debug_printf("forked child %d\n", pid);
	close(channel[1]);
	pf = fdopen(channel[0], "r");
	debug_printf("pipe on FILE *%p\n", pf);
	return pf;
}

/* this version hacked for testing purposes */
/* return code is exit status of the process that is run. */
static int process_command_subs(o_string *dest, struct p_context *ctx, struct in_str *input, int subst_end)
{
	int retcode;
	o_string result = NULL_O_STRING;
	struct p_context inner;
	FILE *p;
	struct in_str pipe_str;
	initialize_context(&inner);

	/* recursion to generate command */
	retcode = parse_stream(&result, &inner, input, subst_end);
	if (retcode != 0) return retcode;  /* syntax error or EOF */
	done_word(&result, &inner);
	done_pipe(&inner, PIPE_SEQ);
	b_free(&result);

	p = generate_stream_from_list(inner.list_head);
	if (p == NULL) return 1;
	mark_open(fileno(p));
	setup_file_in_str(&pipe_str, p);

	/* now send results of command back into original context */
	retcode = parse_stream(dest, ctx, &pipe_str, '\0');
	/* XXX In case of a syntax error, should we try to kill the child?
	 * That would be tough to do right, so just read until EOF. */
	if (retcode == 1) {
		while (b_getch(&pipe_str) != EOF)
			/* discard */;
	}

	debug_printf("done reading from pipe, pclose()ing\n");
	/* This is the step that wait()s for the child.  Should be pretty
	 * safe, since we just read an EOF from its stdout.  We could try
	 * to better, by using wait(), and keeping track of background jobs
	 * at the same time.  That would be a lot of work, and contrary
	 * to the KISS philosophy of this program. */
	mark_closed(fileno(p));
	retcode = pclose(p);
	free_pipe_list(inner.list_head, 0);
	debug_printf("pclosed, retcode=%d\n", retcode);
	/* XXX this process fails to trim a single trailing newline */
	return retcode;
}

static int parse_group(o_string *dest, struct p_context *ctx,
	struct in_str *input, int ch)
{
	int rcode, endch = 0;
	struct p_context sub;
	struct child_prog *child = ctx->child;
	if (child->argv) {
		syntax();
		return 1;  /* syntax error, groups and arglists don't mix */
	}
	initialize_context(&sub);
	switch (ch) {
	case '(':
		endch = ')';
		child->subshell = 1;
		break;
	case '{':
		endch = '}';
		break;
	default:
		syntax();   /* really logic error */
	}
	rcode = parse_stream(dest, &sub, input, endch);
	done_word(dest, &sub); /* finish off the final word in the subcontext */
	done_pipe(&sub, PIPE_SEQ);  /* and the final command there, too */
	child->group = sub.list_head;
	return rcode;
	/* child remains "open", available for possible redirects */
}

/* basically useful version until someone wants to get fancier,
 * see the bash man page under "Parameter Expansion" */
static const char *lookup_param(const char *src)
{
	const char *p = NULL;
	if (src) {
		p = getenv(src);
		if (!p)
			p = get_local_var(src);
	}
	return p;
}

/* Make new string for parser */
static char* make_string(char ** inp)
{
	char *p;
	char *str = NULL;
	int n;
	int len = 2;

	for (n = 0; inp[n]; n++) {
		p = insert_var_value(inp[n]);
		str = xrealloc(str, (len + strlen(p)));
		if (n) {
			strcat(str, " ");
		} else {
			*str = '\0';
		}
		strcat(str, p);
		len = strlen(str) + 3;
		if (p != inp[n]) free(p);
	}
	len = strlen(str);
	str[len] = '\n';
	str[len+1] = '\0';
	return str;
}

/* return code: 0 for OK, 1 for syntax error */
static int handle_dollar(o_string *dest, struct p_context *ctx, struct in_str *input)
{
	int i, advance = 0;
	char sep[] = " ";
	int ch = input->peek(input);  /* first character after the $ */
	debug_printf("handle_dollar: ch=%c\n", ch);
	if (isalpha(ch)) {
		b_addchr(dest, SPECIAL_VAR_SYMBOL);
		ctx->child->sp++;
		while (ch = b_peek(input), isalnum(ch) || ch == '_') {
			b_getch(input);
			b_addchr(dest, ch);
		}
		b_addchr(dest, SPECIAL_VAR_SYMBOL);
	} else if (isdigit(ch)) {
		i = ch - '0';  /* XXX is $0 special? */
		if (i < global_argc) {
			parse_string(dest, ctx, global_argv[i]); /* recursion */
		}
		advance = 1;
	} else switch (ch) {
		case '$':
			b_adduint(dest, getpid());
			advance = 1;
			break;
		case '!':
			if (last_bg_pid > 0) b_adduint(dest, last_bg_pid);
			advance = 1;
			break;
		case '?':
			b_adduint(dest, last_return_code);
			advance = 1;
			break;
		case '#':
			b_adduint(dest, global_argc ? global_argc-1 : 0);
			advance = 1;
			break;
		case '{':
			b_addchr(dest, SPECIAL_VAR_SYMBOL);
			ctx->child->sp++;
			b_getch(input);
			/* XXX maybe someone will try to escape the '}' */
			while (1) {
				ch = b_getch(input);
				if (ch == EOF || ch == '}')
					break;
				b_addchr(dest, ch);
			}
			if (ch != '}') {
				syntax();
				return 1;
			}
			b_addchr(dest, SPECIAL_VAR_SYMBOL);
			break;
		case '(':
			b_getch(input);
			process_command_subs(dest, ctx, input, ')');
			break;
		case '*':
			sep[0] = ifs[0];
			for (i = 1; i < global_argc; i++) {
				parse_string(dest, ctx, global_argv[i]);
				if (i+1 < global_argc)
					parse_string(dest, ctx, sep);
			}
			break;
		case '@':
		case '-':
		case '_':
			/* still unhandled, but should be eventually */
			bb_error_msg("unhandled syntax: $%c", ch);
			return 1;
			break;
		default:
			b_addqchr(dest,'$', dest->quote);
	}
	/* Eat the character if the flag was set.  If the compiler
	 * is smart enough, we could substitute "b_getch(input);"
	 * for all the "advance = 1;" above, and also end up with
	 * a nice size-optimized program.  Hah!  That'll be the day.
	 */
	if (advance) b_getch(input);
	return 0;
}

static int parse_string(o_string *dest, struct p_context *ctx, const char *src)
{
	struct in_str foo;
	setup_string_in_str(&foo, src);
	return parse_stream(dest, ctx, &foo, '\0');
}

/* return code is 0 for normal exit, 1 for syntax error */
static int parse_stream(o_string *dest, struct p_context *ctx,
	struct in_str *input, int end_trigger)
{
	int ch, m;
	int redir_fd;
	redir_type redir_style;
	int next;

	/* Only double-quote state is handled in the state variable dest->quote.
	 * A single-quote triggers a bypass of the main loop until its mate is
	 * found.  When recursing, quote state is passed in via dest->quote. */

	debug_printf("parse_stream, end_trigger=%d\n", end_trigger);
	while ((ch = b_getch(input)) != EOF) {
		m = map[ch];
		next = (ch == '\n') ? 0 : b_peek(input);
		debug_printf("parse_stream: ch=%c (%d) m=%d quote=%d\n",
						ch, ch, m, dest->quote);
		if (m == 0 || ((m == 1 || m == 2) && dest->quote)) {
			b_addqchr(dest, ch, dest->quote);
			continue;
		}
		if (m == 2) {  /* unquoted IFS */
			if (done_word(dest, ctx)) {
				return 1;
			}
			/* If we aren't performing a substitution, treat a newline as a
			 * command separator.  */
			if (end_trigger != '\0' && ch == '\n')
				done_pipe(ctx, PIPE_SEQ);
		}
		if (ch == end_trigger && !dest->quote && ctx->w == RES_NONE) {
			debug_printf("leaving parse_stream (triggered)\n");
			return 0;
		}
		if (m == 2)
			continue;
		switch (ch) {
		case '#':
			if (dest->length == 0 && !dest->quote) {
				while (1) {
					ch = b_peek(input);
					if (ch == EOF || ch == '\n')
						break;
					b_getch(input);
				}
			} else {
				b_addqchr(dest, ch, dest->quote);
			}
			break;
		case '\\':
			if (next == EOF) {
				syntax();
				return 1;
			}
			b_addqchr(dest, '\\', dest->quote);
			b_addqchr(dest, b_getch(input), dest->quote);
			break;
		case '$':
			if (handle_dollar(dest, ctx, input) != 0) return 1;
			break;
		case '\'':
			dest->nonnull = 1;
			while (1) {
				ch = b_getch(input);
				if (ch == EOF || ch == '\'')
					break;
				b_addchr(dest, ch);
			}
			if (ch == EOF) {
				syntax();
				return 1;
			}
			break;
		case '"':
			dest->nonnull = 1;
			dest->quote = !dest->quote;
			break;
		case '`':
			process_command_subs(dest, ctx, input, '`');
			break;
		case '>':
			redir_fd = redirect_opt_num(dest);
			done_word(dest, ctx);
			redir_style = REDIRECT_OVERWRITE;
			if (next == '>') {
				redir_style = REDIRECT_APPEND;
				b_getch(input);
			} else if (next == '(') {
				syntax();   /* until we support >(list) Process Substitution */
				return 1;
			}
			setup_redirect(ctx, redir_fd, redir_style, input);
			break;
		case '<':
			redir_fd = redirect_opt_num(dest);
			done_word(dest, ctx);
			redir_style = REDIRECT_INPUT;
			if (next == '<') {
				redir_style = REDIRECT_HEREIS;
				b_getch(input);
			} else if (next == '>') {
				redir_style = REDIRECT_IO;
				b_getch(input);
			} else if (next == '(') {
				syntax();   /* until we support <(list) Process Substitution */
				return 1;
			}
			setup_redirect(ctx, redir_fd, redir_style, input);
			break;
		case ';':
			done_word(dest, ctx);
			done_pipe(ctx, PIPE_SEQ);
			break;
		case '&':
			done_word(dest, ctx);
			if (next == '&') {
				b_getch(input);
				done_pipe(ctx, PIPE_AND);
			} else {
				done_pipe(ctx, PIPE_BG);
			}
			break;
		case '|':
			done_word(dest, ctx);
			if (next == '|') {
				b_getch(input);
				done_pipe(ctx, PIPE_OR);
			} else {
				/* we could pick up a file descriptor choice here
				 * with redirect_opt_num(), but bash doesn't do it.
				 * "echo foo 2| cat" yields "foo 2". */
				done_command(ctx);
			}
			break;
		case '(':
		case '{':
			if (parse_group(dest, ctx, input, ch) != 0)
				return 1;
			break;
		case ')':
		case '}':
			syntax();   /* Proper use of this character caught by end_trigger */
			return 1;
		default:
			syntax();   /* this is really an internal logic error */
			return 1;
		}
	}
	/* complain if quote?  No, maybe we just finished a command substitution
	 * that was quoted.  Example:
	 * $ echo "`cat foo` plus more"
	 * and we just got the EOF generated by the subshell that ran "cat foo"
	 * The only real complaint is if we got an EOF when end_trigger != '\0',
	 * that is, we were really supposed to get end_trigger, and never got
	 * one before the EOF.  Can't use the standard "syntax error" return code,
	 * so that parse_stream_outer can distinguish the EOF and exit smoothly. */
	debug_printf("leaving parse_stream (EOF)\n");
	if (end_trigger != '\0')
		return -1;
	return 0;
}

static void mapset(const char *set, int code)
{
	while (*set)
		map[(unsigned char)*set++] = code;
}

static void update_ifs_map(void)
{
	/* char *ifs and char map[256] are both globals. */
	ifs = getenv("IFS");
	if (ifs == NULL) ifs = " \t\n";
	/* Precompute a list of 'flow through' behavior so it can be treated
	 * quickly up front.  Computation is necessary because of IFS.
	 * Special case handling of IFS == " \t\n" is not implemented.
	 * The map[] array only really needs two bits each, and on most machines
	 * that would be faster because of the reduced L1 cache footprint.
	 */
	memset(map, 0, sizeof(map)); /* most characters flow through always */
	mapset("\\$'\"`", 3);        /* never flow through */
	mapset("<>;&|(){}#", 1);     /* flow through if quoted */
	mapset(ifs, 2);              /* also flow through if quoted */
}

/* most recursion does not come through here, the exception is
 * from builtin_source() */
static int parse_stream_outer(struct in_str *inp, int flag)
{
	struct p_context ctx;
	o_string temp = NULL_O_STRING;
	int rcode;
	do {
		ctx.type = flag;
		initialize_context(&ctx);
		update_ifs_map();
		if (!(flag & FLAG_PARSE_SEMICOLON) || (flag & FLAG_REPARSING))
			 mapset(";$&|", 0);
		inp->promptmode = 1;
		rcode = parse_stream(&temp, &ctx, inp, '\n');
		if (rcode != 1 && ctx.old_flag != 0) {
			syntax();
		}
		if (rcode != 1 && ctx.old_flag == 0) {
			done_word(&temp, &ctx);
			done_pipe(&ctx, PIPE_SEQ);
			run_list(ctx.list_head);
		} else {
			if (ctx.old_flag != 0) {
				free(ctx.stack);
				b_reset(&temp);
			}
			temp.nonnull = 0;
			temp.quote = 0;
			inp->p = NULL;
			free_pipe_list(ctx.list_head, 0);
		}
		b_free(&temp);
	} while (rcode != -1 && !(flag & FLAG_EXIT_FROM_LOOP));   /* loop on syntax errors, return on EOF */
	return 0;
}

static int parse_string_outer(const char *s, int flag)
{
	struct in_str input;
	setup_string_in_str(&input, s);
	return parse_stream_outer(&input, flag);
}

static int parse_file_outer(FILE *f)
{
	int rcode;
	struct in_str input;
	setup_file_in_str(&input, f);
	rcode = parse_stream_outer(&input, FLAG_PARSE_SEMICOLON);
	return rcode;
}

#if ENABLE_HUSH_INTERACTIVE
/* Make sure we have a controlling tty.  If we get started under a job
 * aware app (like bash for example), make sure we are now in charge so
 * we don't fight over who gets the foreground */
static void setup_job_control(void)
{
	pid_t shell_pgrp;

	saved_task_pgrp = shell_pgrp = getpgrp();
	debug_printf("saved_task_pgrp=%d\n", saved_task_pgrp);
	fcntl(interactive_fd, F_SETFD, FD_CLOEXEC);

	/* If we were ran as 'hush &',
	 * sleep until we are in the foreground.  */
	while (tcgetpgrp(interactive_fd) != shell_pgrp) {
		/* Send TTIN to ourself (should stop us) */
		kill(- shell_pgrp, SIGTTIN);
		shell_pgrp = getpgrp();
	}

	/* Ignore job-control and misc signals.  */
	set_jobctrl_sighandler(SIG_IGN);
	set_misc_sighandler(SIG_IGN);
//huh?	signal(SIGCHLD, SIG_IGN);

	/* We _must_ restore tty pgrp fatal signals */
	set_fatal_sighandler(sigexit);

	/* Put ourselves in our own process group.  */
	setpgrp(); /* is the same as setpgid(our_pid, our_pid); */
	/* Grab control of the terminal.  */
	tcsetpgrp(interactive_fd, getpid());
}
#endif

int hush_main(int argc, char **argv);
int hush_main(int argc, char **argv)
{
	int opt;
	FILE *input;
	char **e;

#if ENABLE_FEATURE_EDITING
	line_input_state = new_line_input_t(FOR_SHELL);
#endif

	/* XXX what should these be while sourcing /etc/profile? */
	global_argc = argc;
	global_argv = argv;

	/* (re?) initialize globals.  Sometimes hush_main() ends up calling
	 * hush_main(), therefore we cannot rely on the BSS to zero out this
	 * stuff.  Reset these to 0 every time. */
	ifs = NULL;
	/* map[] is taken care of with call to update_ifs_map() */
	fake_mode = 0;
	close_me_head = NULL;
#if ENABLE_HUSH_INTERACTIVE
	interactive_fd = 0;
	last_bg_pid = 0;
	job_list = NULL;
	last_jobid = 0;
#endif

	/* Initialize some more globals to non-zero values */
	set_cwd();
	if (ENABLE_FEATURE_EDITING)
		cmdedit_set_initial_prompt();
	else PS1 = NULL;
	PS2 = "> ";

	/* initialize our shell local variables with the values
	 * currently living in the environment */
	e = environ;
	if (e)
		while (*e)
			set_local_var(*e++, 2);   /* without call putenv() */

	last_return_code = EXIT_SUCCESS;

	if (argv[0] && argv[0][0] == '-') {
		debug_printf("\nsourcing /etc/profile\n");
		input = fopen("/etc/profile", "r");
		if (input != NULL) {
			mark_open(fileno(input));
			parse_file_outer(input);
			mark_closed(fileno(input));
			fclose(input);
		}
	}
	input = stdin;

	while ((opt = getopt(argc, argv, "c:xif")) > 0) {
		switch (opt) {
		case 'c':
			global_argv = argv + optind;
			global_argc = argc - optind;
			opt = parse_string_outer(optarg, FLAG_PARSE_SEMICOLON);
			goto final_return;
		case 'i':
			// Well, we cannot just declare interactiveness,
			// we have to have some stuff (ctty, etc)
			/*interactive_fd++;*/
			break;
		case 'f':
			fake_mode++;
			break;
		default:
#ifndef BB_VER
			fprintf(stderr, "Usage: sh [FILE]...\n"
					"   or: sh -c command [args]...\n\n");
			exit(EXIT_FAILURE);
#else
			bb_show_usage();
#endif
		}
	}
#if ENABLE_HUSH_INTERACTIVE
	/* A shell is interactive if the '-i' flag was given, or if all of
	 * the following conditions are met:
	 *    no -c command
	 *    no arguments remaining or the -s flag given
	 *    standard input is a terminal
	 *    standard output is a terminal
	 *    Refer to Posix.2, the description of the 'sh' utility. */
	if (argv[optind] == NULL && input == stdin
	 && isatty(STDIN_FILENO) && isatty(STDOUT_FILENO)
	) {
		saved_tty_pgrp = tcgetpgrp(STDIN_FILENO);
		debug_printf("saved_tty_pgrp=%d\n", saved_tty_pgrp);
		if (saved_tty_pgrp >= 0) {
			/* try to dup to high fd#, >= 255 */
			interactive_fd = fcntl(STDIN_FILENO, F_DUPFD, 255);
			if (interactive_fd < 0) {
				/* try to dup to any fd */
				interactive_fd = dup(STDIN_FILENO);
				if (interactive_fd < 0)
					/* give up */
					interactive_fd = 0;
			}
			// TODO: track & disallow any attempts of user
			// to (inadvertently) close/redirect it
		}
	}
	debug_printf("\ninteractive_fd=%d\n", interactive_fd);
	if (interactive_fd) {
		/* Looks like they want an interactive shell */
		setup_job_control();
		/* Make xfuncs do cleanup on exit */
		die_sleep = -1; /* flag */
		if (setjmp(die_jmp)) {
			/* xfunc has failed! die die die */
			hush_exit(xfunc_error_retval);
		}
#if !ENABLE_FEATURE_SH_EXTRA_QUIET
		printf("\n\n%s hush - the humble shell v0.02\n", BB_BANNER);
		printf("Enter 'help' for a list of built-in commands.\n\n");
#endif
	}
#endif

	if (argv[optind] == NULL) {
		opt = parse_file_outer(stdin);
		goto final_return;
	}

	debug_printf("\nrunning script '%s'\n", argv[optind]);
	global_argv = argv + optind;
	global_argc = argc - optind;
	input = xfopen(argv[optind], "r");
	opt = parse_file_outer(input);

#if ENABLE_FEATURE_CLEAN_UP
	fclose(input);
	if (cwd != bb_msg_unknown)
		free((char*)cwd);
	{
		struct variables *cur, *tmp;
		for (cur = top_vars; cur; cur = tmp) {
			tmp = cur->next;
			if (!cur->flg_read_only) {
				free((char*)cur->name);
				free((char*)cur->value);
				free(cur);
			}
		}
	}
#endif

 final_return:
	hush_exit(opt ? opt : last_return_code);
}
