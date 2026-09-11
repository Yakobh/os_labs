/*
 * Main source file for the lsh shell program.
 *
 * You are free to add functions to this file.
 * If you want to add functions in separate files,
 * you will need to modify CMakeLists.txt to compile
 * your additional files.
 *
 * Add appropriate comments to make your code
 * easier for us to grade.
 *
 * Using assert statements is a good way to catch errors early and make debugging easier.
 * Think of them as mini self-checks that ensure your program behaves as expected.
 * By setting up these guardrails, you're creating a more robust and maintainable solution.
 * So go ahead, sprinkle some asserts in your code; they're your friends in disguise!
 *
 * All the best!
 */
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>


// The <unistd.h> header is your gateway to the OS's process management facilities.
#include <unistd.h>

#include "parse.h"

static void print_cmd(Command *cmd);
static void print_pgm(Pgm *p);
void stripwhite(char *);

// Helpful constants
// useradd manpage specifies 32 bytes as max length for username
#define MAX_USERNAME_LEN 32
#define PROMPT_MAX_LEN MAXPATHLEN + MAX_USERNAME_LEN

#define PIPE_READ 0
#define PIPE_WRITE 1

void sigint_handler(int _signal) {
    // terminate the current child process instead of killing the shell
    // we shouldn't need to actually do anything here, SIGINT should be
    // passed along to all child processes so this empty handler will
    // just simply ignore the SIGINT signal
}

void _close(int fd) {
    if (close(fd) == -1) {
        err(EXIT_FAILURE, "close");
    }
}

int handle_pgm(Pgm *prog, int cmd_idx) {
    int pipefd[2];
    if (prog == NULL)
    {
      return cmd_idx;
    }
    else
    {
        // recurse and call the next program which would be the process piping in information to this one
        int this_cmd_idx = handle_pgm(prog->next, cmd_idx + 1);

        char** pgmlist = prog->pgmlist;
        char* cmd = pgmlist[0];

        // check for built-ins
        if (strcmp("cd", cmd) == 0)
            chdir(pgmlist[1]); // NOTE: pgmlist is a buffer of 50 so we are safe to check index 1 here
        else if (strcmp("exit", cmd) == 0)
            exit(0);

        // run the desired command that isn't a shell built-in
        else {
            // we only want to create a pipe if we are not the last command in the list
            if (this_cmd_idx > 1) {
                if (pipe(pipefd) == -1) {
                    err(EXIT_FAILURE, "pipe");
                }
            }

            // spawn the child process
            pid_t p = fork();
            if (p < 0) {
                err(EXIT_FAILURE, "fork");
            }
            // Child Process
            else if (p == 0) {
                // close read end of the pipe
                if (this_cmd_idx > 1) {
                    _close(pipefd[PIPE_READ]);
                    if (dup2(pipefd[PIPE_WRITE], STDOUT_FILENO) == -1)
                        err(EXIT_FAILURE, "dup2");
                    _close(pipefd[PIPE_WRITE]);
                }

                // call the desired command in a child process with the desired arguments
                execvp(cmd, pgmlist);
            }
            // Parent Process (we will be reading data from the spawned process here)
            else {
                // close write end of the pipe
                if (this_cmd_idx > 1) {
                    _close(pipefd[PIPE_WRITE]);
                    if (dup2(pipefd[PIPE_READ], STDIN_FILENO) == -1)
                        err(EXIT_FAILURE, "dup2");
                    _close(pipefd[PIPE_READ]);
                }
                wait(NULL);
            }
        }
    }

    return cmd_idx;
}

int main(void)
{
  // setup signal handlers
  signal(SIGINT, sigint_handler);

  // keep track of the initial STDIN and STDOUT file descriptors
  int STDIN_ORIG, STDOUT_ORIG;
  STDIN_ORIG = dup(STDIN_FILENO);
  STDOUT_ORIG = dup(STDOUT_FILENO);

  for (;;)
  {
    char *line;

    // collect information for the shell prompt
    char prompt[PROMPT_MAX_LEN];
    char cwd_buf[MAXPATHLEN];

    char *currentuser = getlogin();
    getcwd(cwd_buf, MAXPATHLEN);
    snprintf(prompt, PROMPT_MAX_LEN, "%s %s> ", currentuser, cwd_buf);

    line = readline(prompt);

    // line will return NULL on Ctrl+D (EOF)
    if (line == NULL) {
        exit(0);
    }

    // Remove leading and trailing whitespace from the line
    stripwhite(line);

    // If the stripped line is not blank
    if (*line)
    {
      add_history(line);

      Command cmd;
      if (parse(line, &cmd) == 1)
      {
        // Print the parsed command
        print_cmd(&cmd);
      }
      else
      {
        printf("Parse ERROR\n");
      }

      // recursively handle the desired programs to be executed
      handle_pgm(cmd.pgm, 0);

      // reset the STDIN and STDOUT for this process
      dup2(STDIN_ORIG, STDIN_FILENO);
      dup2(STDOUT_ORIG, STDOUT_FILENO);
    }

    // Free the input buffer
    free(line);
  }

  return 0;
}

/*
 * Print a Command structure as returned by parse on stdout.
 *
 * Helper function, no need to change. Might be useful to study as inspiration.
 */
static void print_cmd(Command *cmd_list)
{
  printf("------------------------------\n");
  printf("Parse OK\n");
  printf("stdin:      %s\n", cmd_list->rstdin ? cmd_list->rstdin : "<none>");
  printf("stdout:     %s\n", cmd_list->rstdout ? cmd_list->rstdout : "<none>");
  printf("background: %s\n", cmd_list->background ? "true" : "false");
  printf("Pgms:\n");
  print_pgm(cmd_list->pgm);
  printf("------------------------------\n");
}

/* Print a linked list of Pgm structures.
 *
 * Helper function, no need to change. It may be useful to study for inspiration.
 */
static void print_pgm(Pgm *p)
{
  if (p == NULL)
  {
    return;
  }
  else
  {
    char **pl = p->pgmlist;

    /* The list is stored in reverse order, so print
     * it in reverse to restore the original order.
     */
    print_pgm(p->next);
    printf("            * [ ");
    while (*pl)
    {
      printf("%s ", *pl++);
    }
    printf("]\n");
  }
}


/* Strip whitespace from the start and end of a string.
 *
 * Helper function, no need to change.
 */
void stripwhite(char *string)
{
  size_t i = 0;

  while (isspace(string[i]))
  {
    i++;
  }

  if (i)
  {
    memmove(string, string + i, strlen(string + i) + 1);
  }

  i = strlen(string) - 1;
  while (i > 0 && isspace(string[i]))
  {
    i--;
  }

  string[++i] = '\0';
}
