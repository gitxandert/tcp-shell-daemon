#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT "3490"

#define BACKLOG 10

void sigchld_handler(int s) {
  (void)s;

  int saved_errno = errno;

  while(waitpid(-1, NULL, WNOHANG) > 0);

  errno = saved_errno;
}

void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int client_shell(int fd) {
  dup2(fd, STDIN_FILENO);
  dup2(fd, STDOUT_FILENO);
  dup2(fd, STDERR_FILENO);

  char line[1024];
  char *argv[100];
  FILE *fp = fdopen(fd, "r");
  if (!fp) {
    perror("fdopen");
    close(fd);
    return 1;
  }  

  while (1) {
    write(STDOUT_FILENO, "xsh> ", 5);
    if (!fgets(line, sizeof(line), fp))
      break;
      
    line[strcspn(line, "\n")] = '\0';
    if (strcmp(line, "exit") == 0) break;

    int argc = 0;
    argv[argc] = strtok(line, " ");
    while (argv[argc] != NULL && argc < 99) 
      argv[++argc] = strtok(NULL, " ");
    argv[argc] = NULL;

    if (argv[0] == NULL) continue;

    pid_t pid = fork();
    if (pid == 0) {

      char ex[128] = "./";
      strcat(ex, argv[0]);
      
      execvp(ex, argv);
      perror("exec failed");
      exit(1);
    } else if (pid > 0) {
      wait(NULL);
    } else {
      perror("fork failed");
    }
  }

  shutdown(fd, SHUT_RDWR);
  fclose(fp);
  close(fd);
  return 0;
}

int main(void) {
  if (setuid(0) < 0) {
    fprintf(stderr, "Run as root\n");
    exit(1);
  }

  char *pwd = malloc(128);
  if (!(getcwd(pwd, 128))) {
    perror("getcwd");
    exit(1);
  }

  // set current working directory as root
  if (chroot(pwd) < 0) {
    perror("chroot");
    exit(1);
  }

  int sockfd, new_fd;
  struct addrinfo hints, *servinfo, *p;
  struct sockaddr_storage their_addr;
  socklen_t sin_size;
  struct sigaction sa;
  int yes = 1;
  char s[INET6_ADDRSTRLEN];
  int rv;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
    return 1;
  }

  for (p = servinfo; p != NULL; p = p->ai_next) {
    if ((sockfd = socket(p->ai_family, p->ai_socktype,
            p->ai_protocol)) == -1) {
      perror("server: socket");
      continue;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
          sizeof(int)) == -1) {
      perror("setsockopt");
      exit(1);
    }

    if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
      close(sockfd);
      perror("server: bind");
      continue;
    }

    break;
  }

  freeaddrinfo(servinfo);

  if (p == NULL) {
    fprintf(stderr, "server: failed to bind\n");
    exit(1);
  }

  if (listen(sockfd, BACKLOG) == -1) {
    perror("listen");
    exit(1);
  }

  sa.sa_handler = sigchld_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if (sigaction(SIGCHLD, &sa, NULL) == -1) {
    perror("sigaction");
    exit(1);
  }

  printf("server: waiting for connections...\n");

  while(1) {
    sin_size = sizeof their_addr;
    new_fd = accept(sockfd, (struct sockaddr*)&their_addr,
        &sin_size);
    if (new_fd == -1) {
      perror("accept");
      continue;
    }

    inet_ntop(their_addr.ss_family,
        get_in_addr((struct sockaddr*)&their_addr),
        s, sizeof s);
    printf("server: got connection from %s\n", s);

    pid_t pid = fork();
    if (pid == 0) {
      close(sockfd);
      client_shell(new_fd);
      close(new_fd);
      exit(0);
    } else if (pid > 0) {
      close(new_fd);
    } else {
      perror("fork failed");
    }
  }

  return 0;
}
