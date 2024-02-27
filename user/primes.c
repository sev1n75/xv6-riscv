#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
  int numcnt, fds_1[2], fds_2[2], fds_3[2], curnum; 
  char tmp[1];

  pipe(fds_1);
  pipe(fds_3);
  if (fork() != 0) { // 排除 2 的倍数
    printf("prime %d\n", 2);
    close(fds_1[0]);
    close(fds_3[1]);
    for (numcnt = 3; numcnt <= 35; numcnt++) {
      if (numcnt % 2 != 0) { // 不是 2 的倍数
        write(fds_1[1], (char*)&numcnt, 4);
      }
    }
    close(fds_1[1]);  // 使得第 33 行可以返回 0
    while(read(fds_3[0], tmp, 1));

  } else {
    close(fds_1[1]);
    close(fds_3[0]);
    pipe(fds_2);  // 先 close 再 pipe

    if (fork() != 0) { // 排除 3 的倍数
      printf("prime %d\n", 3);
      close(fds_2[0]);
      close(fds_3[1]);

      while(read(fds_1[0], (char*)&curnum, 4)) {
        if (curnum % 3 != 0) {
          write(fds_2[1], (char*)&curnum, 4);
        }
      }
      close(fds_2[1]);
    
    } else { // 排除 5 的倍数
      printf("prime %d\n", 5);
      close(fds_1[0]);
      close(fds_2[1]);

      while(read(fds_2[0], (char*)&curnum, 4)) {
        if (curnum % 5 != 0) 
          printf("prime %d\n", curnum);
      }
      close(fds_3[1]);
    }

  }
  
  //printf("%d exit", getpid());
  exit(0);
}
