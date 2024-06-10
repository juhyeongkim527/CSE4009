/*
 * tsh - A tiny shell program with job control
 *
 * Name: Juhyeong Kim
 * Student id: 2021093518
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
  pid_t pid;              /* job PID */
  int jid;                /* job ID [1, 2, ...] */
  int state;              /* UNDEF, BG, FG, or ST */
  char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/*----------------------------------------------------------------------------
 * Functions that you will implement
 */

void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/*----------------------------------------------------------------------------*/

/* These functions are already implemented for your convenience */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv)
{
  char c;
  char cmdline[MAXLINE];
  int emit_prompt = 1; /* emit prompt (default) */

  /* Redirect stderr to stdout (so that driver will get all output
   * on the pipe connected to stdout) */
  dup2(1, 2);

  /* Parse the command line */
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h':             /* print help message */
        usage();
        break;
      case 'v':             /* emit additional diagnostic info */
        verbose = 1;
        break;
      case 'p':             /* don't print a prompt */
        emit_prompt = 0;  /* handy for automatic testing */
        break;
      default:
        usage();
    }
  }

  /* Install the signal handlers */

  /* These are the ones you will need to implement */
  Signal(SIGINT,  sigint_handler);   /* ctrl-c */
  Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
  Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

  /* This one provides a clean way to kill the shell */
  Signal(SIGQUIT, sigquit_handler);

  /* Initialize the job list */
  initjobs(jobs);

  /* Execute the shell's read/eval loop */
  while (1) {

    /* Read command line */
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
      app_error("fgets error");
    if (feof(stdin)) { /* End of file (ctrl-d) */
      fflush(stdout);
      exit(0);
    }

    /* Evaluate the command line */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  }

  exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */
void eval(char *cmdline)
{
  // 파싱된 commandline을 argv에 저장
  char *argv[MAXARGS];
  // background에서 실행되어야 하는지 foreground에서 실행되어야 하는지 확인
  int bg = parseline(cmdline, argv);
  // 자식 프로세스의 pid
  pid_t pid;
  // 시그널 집합
  sigset_t signals;

  //사용자의 입력이 없으면 함수를 return
  if (argv[0] == NULL){
    return;
  }

  // argv가builtin_cmd(quit, jobs, bg, fg)가 아닌 경우 처리
  if (!builtin_cmd(argv)){
    // 시그널을 차단
    sigemptyset(&signals);
    sigaddset(&signals, SIGCHLD);
    sigprocmask(SIG_BLOCK, &signals, NULL);
      
    // 자식 프로세스를 fork하고, 실패하면 에러메시지 출력 후 함수 return
    if((pid = fork()) < 0){
      unix_error("fork error");
      return;
    }

    // 자식 프로세스인 경우
    if (pid == 0){  
      // set id 설정
      setpgid(0,0); 
      // signal 차단 해제
      sigprocmask(SIG_UNBLOCK,&signals, NULL);
        
      if (execve(argv[0], argv, environ) < 0){
        // execve 함수의 오류가 발생한 경우 오류메시지 출력 후 종료
        printf("%s: Command not found\n", argv[0]);
        exit(0);
      }
    }

    // background가 아닌 경우
    if (!bg){
      // foreground job 추가
      addjob(jobs, pid, FG,cmdline);
      // signal 차단 해제
      sigprocmask(SIG_UNBLOCK,&signals, NULL);
      waitfg(pid);
    }

    // background인 경우
    else{
      // background job 추가
      addjob(jobs, pid, BG,cmdline);
      // signal 차단 해제
      sigprocmask(SIG_UNBLOCK,&signals, NULL);
      printf("[%d] (%d) %s", pid2jid(pid), (int)pid, cmdline);
    }
  }
  return;
}
/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv)
{
  static char array[MAXLINE]; /* holds local copy of command line */
  char *buf = array;          /* ptr that traverses command line */
  char *delim;                /* points to first space delimiter */
  int argc;                   /* number of args */
  int bg;                     /* background job? */

  strcpy(buf, cmdline);
  buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
  while (*buf && (*buf == ' ')) /* ignore leading spaces */
    buf++;

  /* Build the argv list */
  argc = 0;
  if (*buf == '\'') {
    buf++;
    delim = strchr(buf, '\'');
  }
  else {
    delim = strchr(buf, ' ');
  }

  while (delim) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* ignore spaces */
      buf++;

    if (*buf == '\'') {
      buf++;
      delim = strchr(buf, '\'');
    }
    else {
      delim = strchr(buf, ' ');
    }
  }
  argv[argc] = NULL;

  if (argc == 0)  /* ignore blank line */
    return 1;

  /* should the job run in the background? */
  if ((bg = (*argv[argc-1] == '&')) != 0) {
    argv[--argc] = NULL;
  }
  return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char **argv)
{
  char *cmd = argv[0];

  // 입력이 quit인 경우 프로그램 종료
  if(!strcmp("quit", cmd)){
    exit(0);
  }
  // 입력이 jobs인 경우 listjobs
  else if(!strcmp("jobs", cmd)){
    listjobs(jobs);
    return 1;
  }
  // 입력이 bg 또는 fg인 경우 bg 또는 fg로 작업 전환
  else if(!strcmp("bg", cmd) || !strcmp("fg", cmd)){
    do_bgfg(argv);
    return 1;
  }

  // &인 경우 1을 리턴
  else if(!strcmp("&", cmd)){  
    return 1;    
  }

  return 0;     /* not a builtin command */
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
{
  struct job_t *job;
  pid_t pid = 0;

  if(argv[1] == NULL){
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
    return;
  }
  
  // command line에서 %(jobid)로 입력된 경우, 
  if(argv[1][0] == '%'){
    // jobid를 jid에 저장
    int jid = atoi(&argv[1][1]);
    // jid에 해당하는 job을 찾아서 저장
    job = getjobjid(jobs, jid);
    
    if(job == NULL){           
      printf ("%s: No such job\n", argv[1]);
      return;
    }

    // job이 존재하는 경우에 pid에 job의 pid를 저장
    else{
      pid = job->pid;
    }
  }
  
  // command line이 숫자(pid)로 시작하는 경우
  else if(isdigit(argv[1][0])){
    // command line을 int인 pid로 변환
    pid = (pid_t) atoi(argv[1]); 
    // pid에 해당하는 job을 찾아서 저장
    job = getjobpid(jobs, pid);

    if(job == NULL){
      printf("(%d): No such process\n", pid);
      return;
    }
  }

  // 에러 메시지 출력
  else{
    printf("%s: argument must be a PID or %%jobid\n", argv[0]);
    return;
  }
  
  // SIGCONT signal으로 작업을 계속 하도록 하고, return 값이 0보다 작으면 에러 출력 
  if(kill(-pid, SIGCONT) < 0){ 
    unix_error("do_bgfg ERROR");
  }

  // bg인 경우
  if (!strcmp(argv[0], "bg")){
    job->state = BG;
    printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
  }
  
  // fg인 경우
  else{
    job->state = FG;
    waitfg(job->pid);
  }
  
  return; 
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
  // pid의 jobpid를 가져와서 struct에 저장
  struct job_t *job = getjobpid(jobs,pid);

  // job이 없는 경우 함수 return
  if (!job) {
    return;
  }

  // job의 pid와 parameter의 pid가 같고, state가 foreground일 때, 1초 sleep
  while(job->pid == pid && job->state == FG) {
    sleep(1);
  }

  return; 
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig)
{
  pid_t pid;
  int status;
  
  // 멈추거나 종료된 자식 process가 있을 때 까지 반복 
  while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0){

    // 자식 process가 정상 종료된 경우
    if(WIFEXITED(status)){
      // 해당 job 삭제
      deletejob(jobs, pid); 
    }
    // 시그널에 의해 종료된 경우
    else if (WIFSIGNALED(status)){
      // job과 signal 정보 출력
      printf("Job [%d] (%d) terminated by signal %d\n",  pid2jid(pid), (int) pid, WTERMSIG(status));
      // 해당 job 삭제
      deletejob(jobs,pid);
    }
    // 자식 process가 멈춘 경우
    else if(WIFSTOPPED(status)){
      // state를 ST(stop)으로 설정
      getjobpid(jobs, pid)->state =ST;
      // job과 signal 정보 출력
      printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), (int) pid, WSTOPSIG(status));
    }
  }
  return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{
  // foreground job의 id를 fg에 저장
  pid_t fg = fgpid(jobs);
  // fg가 존재하는 경우
  if(fg != 0){
    // foreground에 SIGINT signal을 전달하고 return값이 0보다 작으면 에러 출력
    if (kill(-fg, sig) < 0){
      unix_error("SIGINT ERROR");
    }
  }
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
  // foreground job의 id를 fg에 저장
  pid_t fg = fgpid(jobs);
  // fg가 존재하는 경우
  if(fg != 0){
    // foreground에 SIGTSTP signal을 전달하고 return값이 0보다 작으면 에러 출력
    if (kill(-fg,sig) < 0){
      unix_error("SIGSTOP ERROR");
    }
  }
  return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
  job->pid = 0;
  job->jid = 0;
  job->state = UNDEF;
  job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) 
{  
  int i;
  
  for (i = 0; i < MAXJOBS; i++)
    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
  int i, max=0;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
      max = jobs[i].jid;
  return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == 0) {
      jobs[i].pid = pid;
      jobs[i].state = state;
      jobs[i].jid = nextjid++;
      if (nextjid > MAXJOBS)
        nextjid = 1;
      strcpy(jobs[i].cmdline, cmdline);
      if(verbose){
        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
      }
      return 1;
    }
  }
  printf("Tried to create too many jobs\n");
  return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid)
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == pid) {
      clearjob(&jobs[i]);
      nextjid = maxjid(jobs)+1;
      return 1;
    }
  }
  return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].state == FG)
      return jobs[i].pid;
  return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
  int i;

  if (pid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
      return &jobs[i];
  return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid)
{
  int i;

  if (jid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
      return &jobs[i];
  return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid)
{
  int i;

  if (pid < 1)
    return 0;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
      return jobs[i].jid;
    }
  return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs)
{
  int i;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid != 0) {
      printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
      switch (jobs[i].state) {
        case BG:
          printf("Running ");
          break;
        case FG:
          printf("Foreground ");
          break;
        case ST:
          printf("Stopped ");
          break;
        default:
          printf("listjobs: Internal error: job[%d].state=%d ",
              i, jobs[i].state);
      }
      printf("%s", jobs[i].cmdline);
    }
  }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
  printf("Usage: shell [-hvp]\n");
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
  fprintf(stdout, "%s: %s\n", msg, strerror(errno));
  exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
  struct sigaction action, old_action;

  action.sa_handler = handler;
  sigemptyset(&action.sa_mask); /* block sigs of type being handled */
  action.sa_flags = SA_RESTART; /* restart syscalls if possible */

  if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
  return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(1);
}



