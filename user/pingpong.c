#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  int pid, pipefd[2];
  char buf[1];
  pipe(pipefd);

  if (fork()!= 0) {  // parent
    pid = getpid();

    if (write(pipefd[1], "c", 1) <= 0) {
      fprintf(2, "pingpong: write error(parent)\n");
      exit(1);
    }

    sleep(10);

    if (read(pipefd[0], buf, 1) <= 0) {
      fprintf(2, "pingpong: read error(child)\n");
      exit(1);
    }

    printf("%d: received pong\n", pid);

  } else {  // child
    pid = getpid();

    if (read(pipefd[0], buf, 1) <= 0) {
      fprintf(2, "pingpong: read error(child)\n");
      exit(1);
    }

    printf("%d: received ping\n", pid);

    if (write(pipefd[1], buf, 1) <= 0) {
      fprintf(2, "pingpong: write error(child)\n");
      exit(1);
    }
  }
  exit(0);
}
