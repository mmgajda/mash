#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/*TOKENS and SIZE define input buffer size/length */
#define TOKENS 128
#define SIZE 4096

typedef struct command {
  char **argv;
  char *stdout_file;
  char *stdin_file;
  char *stderr_file;
  int append_bool;
  int bground_bool;
  int merge_err;
  struct command *next;
} command_t;

typedef struct {
  int saved_stdin;
  int saved_stdout;
  int saved_stderr;
} fd_backup_t;

typedef void (*builtin_fn)(command_t *cmd);

/*Function that tokenizes the user's input
 * @param char *input, the user's input string
 * @param int *out_argc, number of user's args
 * @return char **tokenize, the tokenized input string*/
char **tokenize(const char *input, int *out_argc) {
  char **args = malloc((TOKENS + 1) * sizeof(char *));
  int argc = 0;
  int len = strlen(input);
  int i = 0;

  while (i < len && argc < TOKENS) {
    int word_pos = 0;
    int overflow = 0;
    char word[SIZE];

    /* Handle whitespace*/
    while (i < len && isspace((unsigned char)input[i])) {
      i++;
    }

    if (i >= len) {
      break;
    }

    while (i < len && !isspace((unsigned char)input[i])) {
      if (word_pos >= SIZE - 1) {
        overflow = 1;
        break;
      }

      if (input[i] == '\'' || input[i] == '"') {
        char quote = input[i++];

        while (i < len && input[i] != quote && word_pos < (SIZE - 1)) {
          if (quote == '"' && input[i] == '\\' && (i + 1) < len &&
              (input[i + 1] == '"' || input[i + 1] == '$' ||
               input[i + 1] == '\\')) {
            word[word_pos++] = input[i + 1];
            i += 2;
          } else {
            word[word_pos++] = input[i++];
          }
        }
        if (i < len && input[i] == quote) {
          i++;
        }

      } else { /* handle unquoted strings*/
        if (input[i] == '\\' && (i + 1) < len && word_pos < (SIZE - 1)) {
          i++;
          word[word_pos++] = input[i++];
        } else {
          word[word_pos++] = input[i++];
        }
      }

      /*Truncate buffer, continue parsing.*/
      if (overflow > 0) {
        fprintf(stderr,
                "Argument too long; truncating. Try to shorten input.\n");
        while (i < len && !isspace((unsigned char)input[i])) {
          i++;
        }
        word_pos = SIZE - 1;
      }
    }
    word[word_pos] = '\0';
    args[argc++] = strdup(word);
  }
  args[argc] = NULL;

  if (out_argc) {
    *out_argc = argc;
  }

  return args;
}

struct command *parse_command(char **tokens, int num_toks) {
  command_t *cmd = calloc(1, sizeof(*cmd));
  if (!cmd) {
    perror("calloc");
    exit(1);
  }
  command_t *current = cmd;

  // New argv list, anything not actionable here is just passed along
  char **argv_buf = malloc((num_toks + 1) * sizeof *argv_buf);
  int argc_buf = 0;

  for (int i = 0; i < num_toks; i++) {

    if (strcmp(tokens[i], "|") == 0 && i + 1 < num_toks) {
      // pipe
      argv_buf[argc_buf] = NULL;
      current->argv = calloc(argc_buf + 1, sizeof *current->argv);
      memcpy(current->argv, argv_buf, (argc_buf + 1) * sizeof *argv_buf);

      current->next = calloc(1, sizeof *current);
      if (!current->next) {
        perror("calloc");
        exit(1);
      }
      current = current->next;
      current->stdin_file = NULL;
      current->stdout_file = NULL;
      current->stderr_file = NULL;
      current->append_bool = 0;
      current->merge_err = 0;
      current->bground_bool = 0;
      current->next = NULL;
      argc_buf = 0;
    } else if ((strcmp(tokens[i], ">") == 0 || strcmp(tokens[i], "1>") == 0) &&
               i + 1 < num_toks) {
      // redir stdout to file, overwrite if exists
      current->stdout_file = strdup(tokens[++i]);
      current->append_bool = 0;
    } else if ((strcmp(tokens[i], ">>") == 0 ||
                strcmp(tokens[i], "1>>") == 0) &&
               i + 1 < num_toks) {
      // redir stdout to file, append if exists
      current->stdout_file = strdup(tokens[++i]);
      current->append_bool = 1;
    } else if (strcmp(tokens[i], "2>") == 0 && i + 1 < num_toks) {
      // redir std err to file, write
      current->stderr_file = strdup(tokens[++i]);
      current->append_bool = 0;
    } else if (strcmp(tokens[i], "2>>") == 0 && i + 1 < num_toks) {
      // redir std err to file, append
      current->stderr_file = strdup(tokens[++i]);
      current->append_bool = 1;
    } else if ((strcmp(tokens[i], "&>") == 0 || strcmp(tokens[i], ">&") == 0) &&
               i + 1 < num_toks) {
      // redir both stdout and stderr to a file, write
      char *f = strdup(tokens[++i]);
      current->stdout_file = f;
      current->stderr_file = f;
      current->append_bool = 0;
    } else if (strcmp(tokens[i], "<") == 0 && i + 1 < num_toks) {
      // redir stdin from a file
      current->stdin_file = strdup(tokens[++i]);
    } else if (strcmp(tokens[i], "2>&1") == 0) {
      // redir stderr same location as stdout
      current->merge_err = 1;
    } else if (strcmp(tokens[i], "&") == 0 && i == num_toks - 1) {
      current->bground_bool = 1;
    } else {
      // just copy the token into argv, do nothing
      argv_buf[argc_buf++] = strdup(tokens[i]);
    } // end if/else block
  } // end for

  argv_buf[argc_buf] = NULL;
  current->argv = calloc(argc_buf + 1, sizeof *current->argv);
  memcpy(current->argv, argv_buf, (argc_buf + 1) * sizeof *argv_buf);

  free(argv_buf);
  for (int j = 0; j < num_toks; j++) {
    free(tokens[j]);
  }
  free(tokens);

  return cmd;
}

int apply_redirects(const command_t *cmd, fd_backup_t *backup) {
  if (backup) {
    backup->saved_stdin = dup(STDIN_FILENO);
    backup->saved_stdout = dup(STDOUT_FILENO);
    backup->saved_stderr = dup(STDERR_FILENO);
  }

  if (cmd->stdin_file) {
    int fd = open(cmd->stdin_file, O_RDONLY);

    if (fd < 0) {
      perror("open stdin");
      return -1;
    }
    dup2(fd, STDIN_FILENO);
    close(fd);
  }

  if (cmd->stdout_file) {
    int flags = O_WRONLY | O_CREAT | (cmd->append_bool ? O_APPEND : O_TRUNC);
    int fd = open(cmd->stdout_file, flags, 0666);

    if (fd < 0) {
      perror("open stdout");
      return -1;
    }

    dup2(fd, STDOUT_FILENO);
    close(fd);
  }

  if (cmd->stderr_file) {
    int flags = O_WRONLY | O_CREAT | (cmd->append_bool ? O_APPEND : O_TRUNC);
    int fd = open(cmd->stderr_file, flags, 0666);

    if (fd < 0) {
      perror("open stderr");
      return -1;
    }
    dup2(fd, STDERR_FILENO);
    close(fd);
  }

  if (cmd->merge_err) {
    dup2(STDOUT_FILENO, STDERR_FILENO);
  }

  return 0;
}

/*Function to restore fds from fd_backup_t, defined at top of file*/
void restore_fds(const fd_backup_t *backup) {
  if (backup->saved_stdin >= 0) {
    dup2(backup->saved_stdin, STDIN_FILENO);
    close(backup->saved_stdin);
  }
  if (backup->saved_stdout >= 0) {
    dup2(backup->saved_stdout, STDOUT_FILENO);
    close(backup->saved_stdout);
  }
  if (backup->saved_stderr >= 0) {
    dup2(backup->saved_stderr, STDERR_FILENO);
    close(backup->saved_stderr);
  }
}

/*Function that acts as a wrapper for builtins, streamlines handling of args*/
void run_builtin(command_t *cmd, builtin_fn fn) {
  fd_backup_t backup = {-1, -1, -1};

  if (apply_redirects(cmd, &backup) < 0) {
    restore_fds(&backup);
    return;
  }

  fn(cmd);

  restore_fds(&backup);
}
void echo_builtin(command_t *cmd) {
  for (int i = 1; cmd->argv[i]; i++) {
    printf("%s%s", cmd->argv[i], cmd->argv[i + 1] ? " " : "");
  }
  printf("\n");
}
void pwd_builtin(command_t *cmd) {
  char cwd[1024];
  if (getcwd(cwd, sizeof(cwd)) != NULL) {
    printf("%s\n", cwd);
  } else {
    perror("Error getting present working directory with getcwd()\n");
  }
}
/*Function to exec external commands, uses execvp*/
void exec_external_cmd(const command_t *cmd) {
  pid_t pid = fork();

  if (pid < 0) {
    perror("Fork failed");
    return;
  } else if (pid == 0) {
    // child process, restore signal defaults
    signal(SIGINT, SIG_DFL);

    if (apply_redirects(cmd, NULL) < 0) {
      _exit(1);
    }
    execvp(cmd->argv[0], cmd->argv);
    fprintf(stderr, "%s: command not found\n", cmd->argv[0]);
    _exit(1);
  } else {
    int status;
    if (!cmd->bground_bool) {
      waitpid(pid, &status, 0);
    } else {
      printf("[background pid %d]\n", pid);
    }
  }
}
void exec_pipeline(command_t *head) {
  // exec each command in the linked list
  // first count list
  int n = 0;
  for (command_t *cmd = head; cmd; cmd = cmd->next) {
    n++;
  }
  if (n == 0)
    return;

  // create n-1 pipes
  int pipes[n - 1][2];
  for (int i = 0; i < (n - 1); i++) {
    if (pipe(pipes[i]) < 0) {
      perror("pipe");
      return;
    }
  }

  command_t *cmd = head;
  for (int i = 0; i < n; i++, cmd = cmd->next) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      return;
    }
    if (pid == 0) {
      // child process
      signal(SIGINT, SIG_DFL);
      if (i > 0) {
        // if not first command, dup2 read end of previous pipe to stdin
        dup2(pipes[i - 1][0], STDIN_FILENO);
      }

      if (i < (n - 1)) {
        // if not last command, dup2 write end of this pipe to stdout
        dup2(pipes[i][1], STDOUT_FILENO);
      }

      // close all pipe fds in the child
      for (int j = 0; j < (n - 1); j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
      }
      apply_redirects(cmd, NULL);
      execvp(cmd->argv[0], cmd->argv);
      _exit(1);
    } // close unused fds in parent
    // wait for children here, unless bground procs
  }
  // close all pipe fds in parent
  for (int i = 0; i < (n - 1); i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }

  // Parent waits for all children, unless running in bground
  if (!head->bground_bool) {
    while (wait(NULL) > 0) {
    }
  }
}

void free_cmd_list(command_t *head) {
  for (command_t *c = head, *next; c; c = next) {
    next = c->next;
    for (int j = 0; c->argv[j]; j++) {
      free(c->argv[j]);
    }
    free(c->argv);
    free(c->stdin_file);

    if (c->stdout_file) {
      free(c->stdout_file);
    }

    if (c->stderr_file && c->stderr_file != c->stdout_file) {
      free(c->stderr_file);
    }

    free(c);
  }
}

/*For autocomplete, disables line buffering and echo*/
void enable_raw_mode() {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &raw);
  raw.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

/*Resumes line buferring, echo, etc upon autocompletion*/
void disable_raw_mode() {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &raw);
  raw.c_lflag |= (ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

void input_completion(char *input, char *builtins[]) {
  int pos = strlen(input);
  int prefix_len;
  char ch;

  enable_raw_mode();
  printf("\r$ %s", input);
  fflush(stdout);

  while ((ch = getchar()) != EOF) {

    // newline means enter was pressed
    if (ch == '\n') {
      input[pos] = '\0';
      putchar('\n');
      break;
    } else if (ch == '\t') {
      input[pos] = '\0';
      prefix_len = pos;
      int found = 0;

      for (int i = 0; builtins[i] != NULL; i++) {
        if (strncmp(builtins[i], input, strlen(input)) == 0) {
          size_t builtins_len = strlen(builtins[i]);
          if (builtins_len + 1 < SIZE) {
            memcpy(input + prefix_len, builtins[i] + prefix_len,
                   builtins_len - prefix_len);
            pos = builtins_len;
            input[pos++] = ' ';
            input[pos] = '\0';
            found = 1;
          }
          break;
        }
      }
      if (!found) {
        char *path_env = getenv("PATH");
        if (path_env) {
          char *path_copy = strdup(path_env);
          char *path_tok = strtok(path_copy, ":");

          while (path_tok && !found) {
            DIR *dir = opendir(path_tok);
            if (dir) {
              struct dirent *ent;
              while ((ent = readdir(dir)) != NULL) {
                if (strncmp(ent->d_name, input, strlen(input)) == 0) {
                  char full_path[SIZE];
                  snprintf(full_path, SIZE, "%s/%s", path_tok, ent->d_name);

                  if (access(full_path, X_OK) == 0) {
                    size_t namelen = strlen(ent->d_name);
                    if (namelen + 2 < SIZE) {
                      memcpy(input, ent->d_name, namelen);
                      pos = namelen;
                      input[pos++] = ' ';
                      input[pos] = '\0';
                      found = 1;
                    }
                    break;
                  }
                }
              }
              closedir(dir);
            }
            path_tok = strtok(NULL, ":");
          }
          free(path_copy);
        }
      }
      if (!found) {
        putchar('\a');
        fflush(stdout);
      } else {
        // overwrite input with autocomplete
        printf("\r$ %s", input);
        fflush(stdout);
      }
    } else if (pos < SIZE - 1) {
      input[pos++] = ch;
      putchar(ch);
      fflush(stdout);
    }
  }
  disable_raw_mode();
}

int main(void) {
  signal(SIGINT, SIG_IGN);

  while (1) {
    setbuf(stdout, NULL);
    char input[SIZE] = {0};
    printf("mash$ ");

    char *builtins[] = {"exit", "type", "echo", "pwd", "cd", NULL};
    int isBuiltIn = 0;
    fflush(stdout);
    input_completion(input, builtins);
    // if (!fgets(input, SIZE, stdin)) {
    // break;
    //}

    size_t len = strlen(input);

    if (len == 0 || (len == 1 && input[0] == '\n')) {
      continue;
    }

    if (input[len - 1] == '\n') {
      input[len - 1] = '\0';
    }

    int argc;
    char **tokens = tokenize(input, &argc);
    command_t *head = parse_command(tokens, argc);
    // TODO: free command lists when no args given and add error logic for when
    // builtins are not passed any arguments
    if (argc == 0) {
    }

    command_t *cmd = head;

    // debug print:
    // printf("Got %d args:\n", argc);
    // for (int j = 0; j < argc; j++) {
    // printf("  argv[%d] = \"%s\"\n", j, argv[j]);
    //}
    for (char **b = builtins; *b; b++) {
      if (strcmp(cmd->argv[0], *b) == 0) {
        isBuiltIn = 1;
        break;
      } else {
        continue;
      }
    }
    if (isBuiltIn) {
      if (strcmp(cmd->argv[0], "exit") == 0) {
        free_cmd_list(head);
        exit(0);
      } else if (strcmp(cmd->argv[0], "echo") == 0) {
        run_builtin(cmd, echo_builtin);
      } else if (strcmp(cmd->argv[0], "pwd") == 0) {
        run_builtin(cmd, pwd_builtin);
      } else if (strcmp(cmd->argv[0], "type") == 0) {
        char *command_type = cmd->argv[1];
        int found = 0;
        for (char **b = builtins; *b; b++) {
          if (strcmp(command_type, *b) == 0) {
            found = 1;
            printf("%s is a shell builtin\n", command_type);
            break;
          }
        }
        if (!found) {
          // find path of binary
          char *path = getenv("PATH");
          char *paths = path ? strdup(path) : NULL;
          int found = 0;

          if (paths) {
            for (char *dir = strtok(paths, ":"); dir; dir = strtok(NULL, ":")) {
              char full_path[SIZE];
              snprintf(full_path, sizeof(full_path), "%s/%s", dir,
                       command_type);
              if (access(full_path, X_OK) == 0) {
                printf("%s is %s\n", command_type, full_path);
                found = 1;
                break;
              }
            }
            free(paths);
            // TODO: free all
          }
          if (!found) {
            fprintf(stderr, "%s not found\n", command_type);
          }
        }

      } else if (strcmp(cmd->argv[0], "cd") == 0) {
        char *target;
        if (cmd->argv[1] == NULL || strcmp(cmd->argv[1], "~") == 0) {
          target = getenv("HOME");
        } else {
          target = cmd->argv[1];
        }

        if (!target || chdir(target) < 0) {
          fprintf(stderr, "cd: %s: %s\n", target, strerror(errno));
        }
        free_cmd_list(head);
        continue;
      }
    } else {
      if (!isBuiltIn && head->next) {
        exec_pipeline(head);
      } else {
        exec_external_cmd(head);
      } // end if/else

      free_cmd_list(head);
      continue;
    }
  } // end main while
} // end main
