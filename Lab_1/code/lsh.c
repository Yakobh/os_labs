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
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>


// The <unistd.h> header is your gateway to the OS's process management facilities.
#include <unistd.h>

#include "parse.h"

//static void print_cmd(Command *cmd);
//static void print_pgm(Pgm *p);
void stripwhite(char *);

// Helpful constants
// useradd manpage specifies 32 bytes as max length for username
#define MAX_USERNAME_LEN 32
#define PROMPT_MAX_LEN MAXPATHLEN + MAX_USERNAME_LEN

#define PIPE_READ 0
#define PIPE_WRITE 1
#define MAX_CHILDREN 1000

// structure for pids belonging to current command/pipeline
// we want to make sure that every process in the pipeline runs before we block
// for example, 'grep hej | ls' should still execute 'ls'.
typedef struct {
  pid_t pids[MAX_CHILDREN];
  size_t count;
  pid_t pgid;
} ChildList;

static void ignore_sigint(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) == -1) {
        err(EXIT_FAILURE, "sigaction(SIGINT)");
    }
}

static void sigchld_handler(int signal_number)
{
    int saved_errno = errno;

    (void)signal_number;

    while (waitpid(-1, NULL, WNOHANG) > 0) {
        // reap every child that has already terminated, preventing zombies
    }

    errno = saved_errno;
}

static void install_sigchld_handler(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = sigchld_handler;
    sigemptyset(&action.sa_mask);

    /*
     * restart readline and other interrupted system calls where possible
     * do not receive notifications merely because a child stopped
     */
    action.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &action, NULL) == -1) {
        err(EXIT_FAILURE, "sigaction(SIGCHLD)");
    }
}

static void wait_for_children(const ChildList *children)
{
    for (size_t i = 0; i < children->count; i++) {
        pid_t result;

        do {
            result = waitpid(children->pids[i], NULL, 0);
        } while (result == -1 && errno == EINTR);

        /*
         * the SIGCHLD handler may already have reaped this child
         * in that case, ECHILD is acceptable
         */
        if (result == -1 && errno != ECHILD) {
            perror("waitpid");
        }
    }
}

static void apply_redirection(const Command *cmd)
{
    int fd;

    // if we have a rstdin we copy that into the stdin_file
    if (cmd->rstdin != NULL) {
        fd = open(cmd->rstdin, O_RDONLY);

        if (fd == -1) {
            perror(cmd->rstdin);
            _exit(127);
        }

        if (dup2(fd, STDIN_FILENO) == -1) {
            perror("dup2");
            _exit(127);
        }

        close(fd);
    }

    // if we have a rstdout we copy that into the stdout_file
    if (cmd->rstdout != NULL) {
        fd = open(cmd->rstdout,
                  O_WRONLY | O_CREAT | O_TRUNC,
                  S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

        if (fd == -1) {
            perror(cmd->rstdout);
            _exit(127);
        }

        if (dup2(fd, STDOUT_FILENO) == -1) {
            perror("dup2");
            _exit(127);
        }

        close(fd);
    }
}

void _close(int fd) {
    if (close(fd) == -1) {
        err(EXIT_FAILURE, "close");
    }
}

int handle_pgm(Pgm *prog, int cmd_idx, Command *cmd, ChildList *children) {
    int background = cmd->background;
    int pipefd[2];
    if (prog == NULL)
    {
      return cmd_idx;
    }
    else
    {
        // recurse and call the next program which would be the process piping in information to this one
        int this_cmd_idx = handle_pgm(prog->next, cmd_idx + 1, cmd, children);

        char** pgmlist = prog->pgmlist;
        char* command_name = pgmlist[0];
        const char* target = pgmlist[1];

        // check for built-ins
        if (strcmp("cd", command_name) == 0) {
          if (target == NULL) {
            target = getenv("HOME");
            if (target == NULL) {
              fprintf(stderr, "cd: HOME not set\n");
              return 1;
            }
          }
          if (chdir(target) != 0) {
              perror("cd");
          }
        } else if (strcmp("exit", command_name) == 0) {
          exit(0);
        }

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
                struct sigaction default_action;

                memset(&default_action, 0, sizeof(default_action));
                default_action.sa_handler = SIG_DFL;
                sigemptyset(&default_action.sa_mask);

                if (sigaction(SIGINT, &default_action, NULL) == -1) {
                  perror("sigaction(SIGINT)");
                  _exit(127);
                }

                // set the process group id to move this process into a different group that will be in the background
                if (background){
                    if (setpgid(0, 0) == -1) {
                      perror("setpgid");
                      _exit(127);
                    }

                }
                // close read end of the pipe
                if (this_cmd_idx > 1) {
                    _close(pipefd[PIPE_READ]);
                    if (dup2(pipefd[PIPE_WRITE], STDOUT_FILENO) == -1)
                        err(EXIT_FAILURE, "dup2");
                    _close(pipefd[PIPE_WRITE]);
                }

                // call the desired command in a child process with the desired arguments
                execvp(command_name, pgmlist);

                perror(command_name);
                _exit(127);
            }
            // Parent Process (we will be reading data from the spawned process here)
            else {
                if (children->count >= MAX_CHILDREN) {
                  errx(EXIT_FAILURE, "too many child processes");
                }

                children->pids[children->count++] = p;

                // close write end of the pipe
                if (this_cmd_idx > 1) {
                    _close(pipefd[PIPE_WRITE]);
                    if (dup2(pipefd[PIPE_READ], STDIN_FILENO) == -1)
                        err(EXIT_FAILURE, "dup2");
                    _close(pipefd[PIPE_READ]);
                }
            }
        }
    }

    return cmd_idx;
}

int main(void)
{
  // setup signal handlers
  // signal(SIGINT, sigint_handler);

  ignore_sigint();
  install_sigchld_handler();

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
      putchar('\n');
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
        //print_cmd(&cmd);
      }
      else
      {
        printf("Parse ERROR\n");
      }

      ChildList children = {
        .count = 0,
        .pgid = 0
      };

      // recursively handle the desired programs to be executed
      apply_redirection(&cmd);
      handle_pgm(cmd.pgm, 0, &cmd, &children);

      if (!cmd.background) {
        wait_for_children(&children);
      }

      // reset the STDIN and STDOUT for this process
      // reset the STDIN and STDOUT for this process
      if (dup2(STDIN_ORIG, STDIN_FILENO) == -1) {
        err(EXIT_FAILURE, "dup2(STDIN_ORIG)");
      }

      if (dup2(STDOUT_ORIG, STDOUT_FILENO) == -1) {
        err(EXIT_FAILURE, "dup2(STDOUT_ORIG)");
      }
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
*/

/* Print a linked list of Pgm structures.
 *
 * Helper function, no need to change. It may be useful to study for inspiration.
static void print_pgm(Pgm *p)
{
  if (p == NULL)
  {
    return;
  }
  else
  {
    char **pl = p->pgmlist;

    // The list is stored in reverse order, so print
    // it in reverse to restore the original order.
    print_pgm(p->next);
    printf("            * [ ");
    while (*pl)
    {
      printf("%s ", *pl++);
    }
    printf("]\n");
  }
}

*/

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
