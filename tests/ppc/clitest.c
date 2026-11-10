/* clitest: a PowerPC command-line tool for checking how Rosettus runs tools from Terminal:
   arguments, working directory, environment, standard input / output / error, exit status.
   Build: gcc -arch ppc -O1 -mmacosx-version-min=10.4 -o clitest clitest.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    char cwd[1024], line[256];
    int i, n = 0, code = 0;
    unsigned long sum = 0;
    for (i = 1; i < argc; i++) printf("arg%d=%s\n", i, argv[i]);
    printf("cwd=%s\n", getcwd(cwd, sizeof cwd) ? cwd : "?");
    printf("CLITEST_VAR=%s\n", getenv("CLITEST_VAR") ? getenv("CLITEST_VAR") : "(unset)");
    printf("stdin is %s\n", isatty(0) ? "a terminal" : "not a terminal");
    if (!isatty(0)) while (fgets(line, sizeof line, stdin)) { n++; sum += strlen(line); }
    printf("stdin lines=%d bytes=%lu\n", n, sum);
    fprintf(stderr, "this line went to stderr\n");
    if (argc > 1 && !strncmp(argv[argc - 1], "exit=", 5)) code = atoi(argv[argc - 1] + 5);
    return code;
}
