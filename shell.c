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


struct command {
    const char **argv;
};

void print_directory() {
    char host[BUFFER_MAX];
    gethostname(host, BUFFER_MAX);
    char *user = getlogin();
    char *dir = getcwd(NULL, 0);

    // Get only the current directory name instead of full path
    int len = strlen(dir);
    char *cdir = dir + len;
    while (*--cdir != '/');
    cdir++;

    printf("\r{%s@%s %s}$ ", user, host, cdir);

    free(dir);
}

// Parse a whitespace separated string into a list of arguments
// The list is allocated dynamically with size MAX_ARGS + 1
// Make sure to call free
// We avoid the use of strtok so as to not override its static buffer
int parse_command(char *str, struct command *cmd) {
    int argc = 0;
    cmd->argv = malloc((MAX_ARGS + 1)* sizeof(char *));

    // Flag for " character
    int flag = 0;

    char *iter = str;
    // Clear leading whitespaces
    while (*iter != '\0' && *iter == ' ') iter++;
    cmd->argv[argc++] = iter++;

    while (*iter != '\0') {
        if (*iter == '\"')
            flag = ~flag;

        if (*iter != ' ') {
            iter++;
            continue;
        }

        // Whitespace within quotes or escaped
        if (*iter == ' ' && (flag || (iter > str && *(iter - 1) == '\\'))) {
            iter++; 
            continue;
        } else { 
            // Token found; end string and clear all 
            // whitespaces till next token
            *iter++ = '\0';
            while (*iter != '\0' && *iter == ' ') iter++;

            if (argc >= MAX_ARGS)
                die("Maximum argument number surpassed");

            cmd->argv[argc++] = iter;
        }
    }


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
    }

    return buf;
}

// Check if called argument is a builtin and act accordingly
// Return -1 when command is not builtin
int handle_builtin(const char** argv, int fdi, int fdo) {
#ifdef DEBUG
    printf("handle_builtin() called\n");
    printf("argv[0]: %s\n", argv[0]);
#endif

    int bi_flag = -1;

    if (fdi != STDIN_FILENO)
        if(dup2(fdi, STDIN_FILENO) == -1) 
            die("dup2");

    if (fdo != STDOUT_FILENO)
        if(dup2(fdo, STDOUT_FILENO) == -1)
            die("dup2");


    if (strcmp(argv[0], "cd") == 0) {
        char *dir = get_dir(argv[1]);

        if (chdir(dir) == -1)
            die("chdir");

        free(dir);

        bi_flag = 0;
    }

    if (strcmp(argv[0], "pwd") == 0) {
        char *dir = getcwd(NULL, 0);
        if (!dir) 
            die("getcwd");

        printf("%s\n", dir);
        free(dir);

        bi_flag = 0;
    }

    if (strcmp(argv[0], "exit") == 0) {
#ifdef DEBUG
        printf("pwd detected");
#endif
        exit(EXIT_SUCCESS);
    }

    if (fdi != STDIN_FILENO)
        if(close(fdi) == -1) 
            die("close");

    if (fdo != STDOUT_FILENO)
        if(close(fdo) == -1)
            die("close");

    return bi_flag;
}


// Spawn a subprocess for the non-builtin command
pid_t spawn_proc(struct command *cmd, int fdi, int fdo) {
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
        if (fdi != STDIN_FILENO)
            if(dup2(fdi, STDIN_FILENO) == -1) 
                die("dup2");

        if (fdo != STDOUT_FILENO)
            if(dup2(fdo, STDOUT_FILENO) == -1)
                die("dup2");

        if (execvp(cmd->argv[0], (char *const *)cmd->argv) == -1)
            die("execvp");
    }

    return p;
}

int pipeline(char *input) {
    // I/O file descriptors
    int fdi = STDIN_FILENO, fdo = STDOUT_FILENO;
    int pfd_next[2];

    pid_t p;
    struct command cmd;

    // Divide the input into segments separated by |
    char *iter = strtok(input, "|");
    char *next = strtok(NULL, "|");

#ifdef DEBUG
    printf("pipeline()\n");
    printf("iter: %s\n", iter);
    printf("next: %s\n", next);
#endif

    while (iter) {
#ifdef DEBUG
        printf("entered while(iter) loop with iter = %u\n", iter);
#endif

        // If there exists a next pipeline stage, make a new pipe
        if (next) {
            if (pipe(pfd_next) == -1)
                die("pipe");

            fdo = pfd_next[0];
            parse_command(iter, &cmd);
            if (handle_builtin(cmd.argv, fdi, fdo) == -1) {
                p = spawn_proc(&cmd, fdi, fdo);
                waitpid(p, NULL, WUNTRACED | WCONTINUED);
            }

            // Initialize read end for next process
            fdi = pfd_next[1];
        } else {
            parse_command(iter, &cmd);
            if (handle_builtin(cmd.argv, fdi, fdo) == -1) {
                p = spawn_proc(&cmd, fdi, fdo);
                waitpid(p, NULL, WUNTRACED | WCONTINUED);
            }
        }

        free(cmd.argv);

        iter = next;
        next = strtok(NULL, "|");
    }
    return 0;
}

int main(void) {
    static char *line = (char *)NULL;
    while (1) {
        print_directory();

        if (line) {
            free(line);
            line = (char *)NULL;
        }
        line = readline("");

        // Ignore empty input
        if (!(line && *line))
            continue;

        add_history(line);

        pipeline(line);
        

    }
    return 0;
}
