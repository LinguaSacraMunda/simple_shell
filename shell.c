#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/types.h>
#include <pwd.h>

#define die(x) {perror(x); exit(EXIT_FAILURE);}
#define MAX_ARGS 64
#define BUFFER_MAX 1024

#define MASK_AP 0x1<<0
#define MASK_QT 0x1<<1

struct command {
    const char **argv;
};

char *print_directory() {
    char host[BUFFER_MAX / 2];
    gethostname(host, BUFFER_MAX / 2);
    char *user = getlogin();
    char *dir = getcwd(NULL, 0);

    // Get only the current directory name instead of full path
    int len = strlen(dir);
    char *cdir = dir + len;
    while (*--cdir != '/');
    cdir++;


    char *out = malloc((strlen(user) + strlen(host) + strlen(cdir) + 16) * sizeof(char));
    sprintf(out, "\r{%s@%s %s}$ ", user, host, cdir);

    free(dir);

    return out;
}

// Parse a whitespace separated string into a list of arguments
// The list is allocated dynamically with size MAX_ARGS + 1
// Make sure to call free
// We avoid the use of strtok so as to not override its static buffer
int parse_command(char *str, struct command *cmd) {
    int argc = 0;
    // Extra pointer for NULL termination
    cmd->argv = malloc((MAX_ARGS + 1)* sizeof(char *));

    // Flag for: ", '
    char flag = 0;

    char *iter = str, *hlpr = str;
    // Clear leading whitespaces
    while (*iter != '\0' && *iter == ' ') iter++;

    // Initialize flag and first token
    if (*iter == '\"')
        flag ^= MASK_QT;

    if (*iter == '\'')
        flag ^= MASK_AP;

    cmd->argv[argc++] = iter++;

    while (*iter != '\0') {
        // Update flag for unescaped quotation marks, outside of apostrophes
        if (*iter == '\"' && !(flag & MASK_AP) && !(iter > str && *(iter - 1) == '\\'))
            flag ^= MASK_QT;

        // Update flag for unescaped apostrophes, outside of quotation marks
        if (*iter == '\'' && !(flag & MASK_QT) && !(iter > str && *(iter - 1) == '\\'))
            flag ^= MASK_AP;

        if (*iter != ' ') {
            iter++;
            continue;
        }

        // Whitespace within quotes or escaped
        if (*iter == ' ' && (flag || (iter > str && *(iter - 1) == '\\'))) {
            iter++; 
            continue;
        } else { 
            // Token found; end string, remove enclosing 
            // quotation marks or apostrophes, and clear all 
            // whitespaces till next token
            if ((*hlpr == '\"' && *(iter - 1) == '\"')
                ||(*hlpr == '\'' && *(iter - 1) == '\'')) {
                *hlpr = '\0';
                *(iter - 1) = '\0';
                cmd->argv[argc - 1]++;
            }

            *iter++ = '\0';
            while (*iter != '\0' && *iter == ' ') iter++;

            if (*iter != '\0') {
                if (argc >= MAX_ARGS)
                    die("Maximum argument number surpassed");

                hlpr = iter;
                cmd->argv[argc++] = iter;
            }
        }
    }

    if ((*hlpr == '\"' && *(iter - 1) == '\"')
        ||(*hlpr == '\'' && *(iter - 1) == '\'')) {
        *hlpr = '\0';
        *(iter - 1) = '\0';
        cmd->argv[argc - 1]++;
    }

    if (argc > MAX_ARGS)
        die("Maximum argument number surpassed");

    cmd->argv[argc] = NULL;

#ifdef DEBUG
    printf("str: %s\n", str);
    for (int i = 0; i < argc; i++) {
        printf("cmd->argv[%d]: ", i);
        for (const char *s = cmd->argv[i]; *s != '\0'; s++) printf("%c", *s);
        printf("\n");
    }
#endif

    return 0;
}

// Expand ~ in path and handle empty paths
char *get_dir(const char *path) {
    char *home = getenv("HOME");
    int len = strlen(path);
    char *buf = malloc(BUFFER_MAX * sizeof(char));


    if (len == 0 || (len == 1 && *path == '~')){
        sprintf(buf, "%s", home);
    } else if (len > 1 && *path == '~') {
        // Substiture ~ with $HOME
        sprintf(buf, "%s%s", home, path + 1);
    } else {
        sprintf(buf, "%s", path);
    }

    return buf;
}

void route_stdio_fd(int fdr, int fdw) {
#ifdef DEBUG
    printf("route_stdio_fd(fdr = %d, fdw = %d)\n", fdr, fdw);
#endif
    if (fdr != STDIN_FILENO)
        if(dup2(fdr, STDIN_FILENO) == -1) 
            die("dup2");

    if (fdw != STDOUT_FILENO)
        if(dup2(fdw, STDOUT_FILENO) == -1)
            die("dup2");
}

void close_pipe_fd(int fdr, int fdw) {
#ifdef DEBUG
    printf("close_pipe_fd(fdr = %d, fdw = %d)\n", fdr, fdw);
#endif
    if (fdr != STDIN_FILENO)
        if(close(fdr) == -1) 
            die("close");

    if (fdw != STDOUT_FILENO)
        if(close(fdw) == -1)
            die("close");
}

// Check for and execute builting commands
// Return 0 if command is builtin; else -1
//
// Currently supported builtins:
//  cd, pwd, exit
int handle_builtin(const char** argv, int fdr, int fdw) {
    int bi_flag = -1;

    if (strcmp(argv[0], "cd") == 0) {
        char *dir = (char *)NULL;
        if (argv[1])
            dir = get_dir(argv[1]);
        else
            dir = get_dir("~");

        if (chdir(dir) == -1)
            printf("cd: no such file or directory: %s\n", dir);
        free(dir);

        bi_flag = 0;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        char *dir = getcwd(NULL, 0);
        if (!dir) 
            die("getcwd");

        write(fdw, dir, sizeof(dir));
        free(dir);

        bi_flag = 0;
    }

    if (strcmp(argv[0], "exit") == 0) {
        exit(EXIT_SUCCESS);
    }

    return bi_flag;
}


// Spawn a subprocess for the non-builtin command
pid_t spawn_proc(struct command *cmd, int fdr, int fdw) {
#ifdef DEBUG
    printf("spawn_proc() called\n");
    for (int i = 0; *(cmd->argv + i) != NULL; i++)
        printf("argv[%d] = %s\n", i, *(cmd->argv + i));
#endif
    

    pid_t p = fork();
    if (p == -1)
        die("fork");

    if (p == 0) {
        // Redirect stdin and stdout to given pipes respectively
        route_stdio_fd(fdr, fdw);

        if (execvp(cmd->argv[0], (char *const *)cmd->argv) == -1) {
            printf("shell: command not found: %s\n", cmd->argv[0]);
            exit(EXIT_FAILURE);
        }
    }

    return p;
}

int pipeline(char *input) {
    // I/O file descriptors
    int fdr = STDIN_FILENO, fdw = STDOUT_FILENO;
    int pfd_next[2];

    pid_t p;
    struct command cmd;


    // Flag for: \" and \'
    char flag = 0;

    // Divide the input into segments separated by |
    //char *iter = strtok(input, "|");
    //char *next = strtok(NULL, "|");

    char *iter = input, *next = input;


    while (*iter != '\0') {
        while (*next != '\0') {
            // Update flag for unescaped quotation marks, outside of apostrophes
            if (*next == '\"' && !(flag & MASK_AP) && !(next > input && *(next - 1) == '\\'))
                flag ^= MASK_QT;

            // Update flag for unescaped apostrophes, outside of quotation marks
            if (*next == '\'' && !(flag & MASK_QT) && !(next > input && *(next - 1) == '\\'))
                flag ^= MASK_AP;

            if (*next != '|') {
                next++;
                continue;
            }

            // Pipe symbol withing quotes or escaped
            if (*next == '|' && (flag || (next > input && *(next - 1) == '\\'))) {
                next++;
                continue;
            } else {
                *next++ = '\0';
                break;
            }
        }

        // If there exists a next pipeline stage, make a new pipe
        if (*next != '\0') {
            if (pipe(pfd_next) == -1)
                die("pipe");
#ifdef DEBUG
            printf("read end :: pfd[0]: %d\n", pfd_next[0]);
            printf("write end :: pfd[1]: %d\n", pfd_next[1]);
#endif

            fdw = pfd_next[1];
            parse_command(iter, &cmd);
            if (handle_builtin(cmd.argv, fdr, fdw) == -1) {
                p = spawn_proc(&cmd, fdr, fdw);
                waitpid(p, NULL, WUNTRACED | WCONTINUED);
            }

            // Initialize read end for next process
            close_pipe_fd(fdr, fdw);
            fdr = pfd_next[0];
        } else {
            parse_command(iter, &cmd);
            if (handle_builtin(cmd.argv, fdr, STDOUT_FILENO) == -1) {
                p = spawn_proc(&cmd, fdr, STDOUT_FILENO);
                waitpid(p, NULL, WUNTRACED | WCONTINUED);
            }
        }

        free(cmd.argv);

        iter = next;
        //next = strtok(NULL, "|");
    }

    // Close read end of last pipe
    close_pipe_fd(fdr, STDOUT_FILENO);
    return 0;
}

int main(void) {
    static char *line = (char *)NULL;
    char *header = (char *)NULL;
    while (1) {
        header = print_directory();

        if (line) {
            free(line);
            line = (char *)NULL;
        }
        line = readline(header);
        free(header);

        // Ignore empty input
        if (!(line && *line))
            continue;

        add_history(line);

        pipeline(line);
        

    }
    return 0;
}
