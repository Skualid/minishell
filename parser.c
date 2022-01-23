#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "parser.h"

#define DEBUG 0

#define MIN(a,b) ((a) < (b) ? (a) : (b))

static int ntokens = 0;
static char ** tokens = NULL;
static tline line;

static void
add_token(char * start, char * end) {
	tokens = realloc(tokens, (ntokens+1)*sizeof(char*));
	if (!tokens) {
		perror("Fatal Error");
		exit(1);
	}
	tokens[ntokens] = malloc(end-start+2);
	if (!tokens[ntokens]) {
		perror("Fatal Error");
		exit(1);
	}
	strncpy(tokens[ntokens], start, end-start+1);
	tokens[ntokens][end-start+1] = 0;
	ntokens++;
}

static int
issymbol(char c) {
	if (c=='<' || c=='>' || c=='|' || c=='&') {
		return 1;
	} else {
		return 0;
	}
}

static void
cleanup(void) {
	int i,j;

	if (line.redirect_input) {
		free(line.redirect_input);
	}
	if (line.redirect_output) {
		free(line.redirect_output);
	}
	if (line.redirect_error) {
		free(line.redirect_error);
	}
	if (line.commands) {
		for (i=0; i<line.ncommands; i++) {
			if (line.commands[i].filename) {
				free(line.commands[i].filename);
			}
			for (j=0; j<line.commands[i].argc; j++) {
				if (line.commands[i].argv[j]) {
					free(line.commands[i].argv[j]);
				}
			}
			if (line.commands[i].argv) {
				free(line.commands[i].argv);
			}
		}
		free(line.commands);
	}
	line.ncommands = 0;
	line.commands = NULL;
	line.redirect_input = NULL;
	line.redirect_output = NULL;
	line.redirect_error = NULL;
	line.background = 0;
}

static void
fill_tokens(char * str) {
	char *start;

	ntokens = 0;
	while (1) {
		while (isspace(*str)) {
			str++;
		}
		if (!*str) {
			break;
		}
		if (issymbol(*str)) {
			add_token(str, str);
			str++;
			continue;
		}
		start = str;
		while (*str && !issymbol(*str) && !isspace(*str)) {
			str++;
		}
		add_token(start, str-1);
	}
}

static int
check_syntax(void) {
	int i;
	int input=0, output=0, error=0, back=0, tub=0;

	for (i=0; i<ntokens; i++) {
		if (tokens[i][0]=='<') {
			if ((input || tub || (i==0) || (i==ntokens-1) || issymbol(tokens[i+1][0]))) {
				return 0;
			}
			input=1;
		}
		if ((tokens[i][0]=='>') && (i+1 < ntokens) && (tokens[i+1][0]=='&')) {
			if ((error) || (i==0) || (i==ntokens-1)) {
				return 0;
			}
			error=1;
		} else if (tokens[i][0]=='>'){
			if ((output) || (i==0) || (i==ntokens-1) || issymbol(tokens[i+1][0])) {
				return 0;
			}
			output=1;
		} 
		if ((tokens[i][0]=='&') && ((i == 0) || (tokens[i-1][0]!='>'))) {
			if (back) {
				return 0;
			}
			back=1;
		} 
		if (tokens[i][0]=='|') {
			if ((output) || (error) || (i==0) || (i==ntokens-1) || issymbol(tokens[i+1][0])) {
				return 0;
			}
			tub=1;
		}
		/*if (issymbol(tokens[i][0])) {
			if (i==0 || i==ntokens-1) {
				return 0;
			}
			if (issymbol(tokens[i+1][0])) {
				return 0;
			}
		}*/
	}
	return 1;
}

static char *
cmd2path(char * cmd) {
	int i;
	char buf[1024];
	char * str;

	if (strchr(cmd, '/')) {
		if (!access(cmd, X_OK))
			return strdup(cmd);
		else
			return NULL;
	}
	str = getenv("PATH");
	if (!str) {
		str = "/bin:/usr/bin";
	}
	while (1) {
		for (i=0; *str && *str!=':'; str++, i++) {
			buf[i] = *str;
		}
		buf[i] = 0;
#if DEBUG
		printf("PATH: %s\n", buf);
#endif
		strcat(buf, "/");
		strcat(buf, cmd);
#if DEBUG
		printf(" File: %s\n", buf);
#endif
		if (!access(buf, X_OK)) {
			return strdup(buf);
		}
		if (!*str) {
			break;
		}
		str++;
	}
	return NULL;
}

tline *
tokenize(char str[]) {
	int i;

	cleanup();
	fill_tokens(str);

	if (!check_syntax()) {
		fprintf(stderr, "Syntax error checking.\n");
		return NULL;
	}

#if DEBUG
	{ /* DEBUG */
		printf("%d tokens:\n", ntokens);
		for (i=0; i<ntokens; i++) {
			printf("\t%s\n", tokens[i]);
		}
	}
#endif
	line.ncommands = 0;
	line.redirect_input = line.redirect_output = line.redirect_error = NULL;
	line.background = 0;
	for (i=0; i<ntokens-1; i++) {
		if (tokens[i][0]=='<') {
			if (line.redirect_input) {
				fprintf(stderr, "Syntax error.\n");
				return NULL;
			}
			line.redirect_input = tokens[i+1];
			free(tokens[i]);
			memmove(&tokens[i], &tokens[i+2], (ntokens-i-2)*sizeof(char*));
			ntokens-=2; i-=2;
		}
	}
	for (i=0; i<ntokens-1; i++) {
		if ((tokens[i][0]=='>') && (tokens[i+1][0]=='&')) {
			if (line.redirect_error) {
				fprintf(stderr, "Syntax error.\n");
				return NULL;
			}
			line.redirect_error = tokens[i+2];
			free(tokens[i]);
			free(tokens[i+1]);	
			memmove(&tokens[i], &tokens[i+3], (ntokens-i-3)*sizeof(char*));
			ntokens-=3; i-=3;
			//fprintf(stderr,"i: %d\n",i);
			if (i < -1)
			  i = -1;
		} else if (tokens[i][0]=='>') {
			if (line.redirect_output) {
				fprintf(stderr, "Syntax error.\n");
				return NULL;
			}
			line.redirect_output = tokens[i+1];
			free(tokens[i]);
			memmove(&tokens[i], &tokens[i+2], (ntokens-i-2)*sizeof(char*));
			ntokens-=2; i-=2;
		}
	}
	for (i=0; i<ntokens; i++) {
		if (tokens[i][0]=='&') {
			if (line.background) {
				fprintf(stderr, "Syntax error.\n");
				return NULL;
			}
			line.background = 1;
			free(tokens[i]);
			if (i < ntokens -1){
				memmove(&tokens[i], &tokens[i+1], (ntokens-i-1)*sizeof(char*));
			} 
			ntokens-=1; i-=1;
		}
	}
	
	line.ncommands = 0;
	line.commands = NULL;
	for (i=0; i<ntokens; i++) {
		if (i==0 || tokens[i-1]==NULL) {
			line.commands = realloc(line.commands, (line.ncommands+1)*sizeof(tcommand));
			if (!line.commands) {
				perror("Fatal Error");
				exit(1);
			}
			line.commands[line.ncommands].argc = 0;
			line.commands[line.ncommands].argv = malloc(sizeof(char*));
			if (!line.commands[line.ncommands].argv) {
				perror("Fatal Error");
				exit(1);
			}
			line.commands[line.ncommands].argv[0] = NULL;
			line.ncommands++;
		}
		if (tokens[i][0]=='|') {
			free(tokens[i]);
			tokens[i] = NULL;
		} else {
			line.commands[line.ncommands-1].argv = realloc(line.commands[line.ncommands-1].argv, (line.commands[line.ncommands-1].argc+2)*sizeof(char*));
			if (!line.commands[line.ncommands-1].argv) {
				perror("Fatal Error");
				exit(1);
			}
			line.commands[line.ncommands-1].argv[line.commands[line.ncommands-1].argc] = tokens[i];
			line.commands[line.ncommands-1].argv[line.commands[line.ncommands-1].argc+1] = NULL;
			line.commands[line.ncommands-1].argc++;
		}
	}
	for (i=0; i<line.ncommands; i++) {
		line.commands[i].filename = cmd2path(line.commands[i].argv[0]);
		/*if (!line.commands[i].filename) {
			perror("Fatal Error");
			exit(1);
		}*/
	}
#if DEBUG
	{ /* DEBUG */
		int a,b;
		printf("INPUT: %s\n", line.redirect_input);
		printf("OUTPUT: %s\n", line.redirect_output);
		printf("ERROR: %s\n", line.redirect_error);
		printf("NCOMMANDS: %d\n", line.ncommands);
		for (a=0; a<line.ncommands; a++) {
			for (b=0; line.commands[a].argv[b]; b++) {
				printf(" %s", line.commands[a].argv[b]);
			}
			printf("\n");
		}
	}
#endif
	return &line;
}
