#include <sys/types.h>
#include <sys/ioctl.h>

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <histedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "slsctl.h"

#define MAX_ARGC (32)

void
usage(void)
{
	printf("Usage: slsctl <command> [command args]\n");
	printf("Commands: partadd partdel attach checkpoint restore listsnaps mountsnap\n");
}

struct command {
	const char *name;
	const char *shorthand;
	void(*usage)();
	int(*cmd)(int, char **);
} commandtable[] = {
	{ "partadd",	"pa",	&partadd_usage,	    &partadd_main},
	{ "partdel",	"pd",	&partdel_usage,	    &partdel_main},
	{ "attach",	"at",	&attach_usage,	    &attach_main},
	{ "checkpoint",	"ch",	&checkpoint_usage,  &checkpoint_main },
	{ "restore",	"re",	&restore_usage,	    &restore_main },
	{ "listsnaps",	"ls",	&listsnaps_usage,   &listsnaps_main },
	{ "mountsnap",	"ms",	&mountsnap_usage,   &mountsnap_main },
	{ "epoch",	"ep",	&epoch_usage,	    &epoch_main},
	{ "spawn",	"sp",	&spawn_usage,	    &spawn_main},
	{ NULL,		NULL,	NULL,		    NULL }
};

static int
slsmatch(const char *candidate, const struct command *cmd)
{
	return ((strcmp(candidate, cmd->name) == 0) ||
		(strcmp(candidate, cmd->shorthand) == 0));

}

static int
slscommand(int argc, char *argv[])
{
	int i;

	assert(argc > 0);

	/* Search for the command. */
	for (int i = 0; commandtable[i].name != NULL; i++) {
		if (slsmatch(argv[0], &commandtable[i]))
			return commandtable[i].cmd(argc, (char **) argv);
	}

	/* No matches. */
	usage();

	return (EINVAL);
}

static char *
slsprompt(EditLine *el)
{
	return ("slsctl> ");
}

static void
slscli(void)
{
	const char **argv;
	HistEvent ev;
	int num, idx;
	int status;
	int argc;

	History *hist = history_init();
	history(hist, &ev, H_SETSIZE, 100);

	 EditLine *el = el_init("slsctl", stdin, stdout, stderr);

	/* Editing mode to use. */
	el_set(el, EL_EDITOR, "vi");

	/* History function to use. */
	el_set(el, EL_HIST, history, hist);

	/* Function that displays the prompt. */
	el_set(el, EL_PROMPT, slsprompt);

	for (;;) {
		/* Get the line. */
		const char *line = el_gets(el, &num);

		/* Tokenize the next line. */
		Tokenizer *tok = tok_init(NULL);
		argc = MAX_ARGC;
		status = tok_str(tok, line, &argc, &argv);
		if (status != 0) {
			fprintf(stderr, "Parsing error %d\n", status);
			tok_end(tok);
			continue;
		}

		assert(argc >= 0);
		/* Empty line, we're done. */
		if (argc == 0) {
			tok_end(tok);
			break;
		}

		/* Run the command. */
		slscommand(argc, (char **) argv);
		tok_end(tok);
	}

	el_end(el);
	history_end(hist);
}

int
main(int argc, const char *argv[])
{
	/* Drop into the CLI if not arguments. */
	if (argc == 1) {
		slscli();
		return (0);
	}

	if (slscommand(argc - 1, (char **) argv + 1) != 0)
		usage();

	return (0);
}

