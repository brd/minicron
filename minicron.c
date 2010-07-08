#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* 
 * the following constants control the behavior of kill_pid() - they serve as second argument
 * after we send SIGTERM to a process, we wait some time before sending SIGKILL
 * the supervisor will wait KILL_TIMEOUT_CHILD seconds for the child to die after SIGTERM
 * the main loop will wait KILL_TIMEOUT_SUPERVISOR seconds for the supervisor to die after SIGTERM 
 * if we have 0 here, we won't send SIGKILL at all (not recommended for KILL_TIMEOUT_CHILD)
 */
#define KILL_TIMEOUT_SUPERVISOR 0 /* the supervisor catches SIGTERM and invokes kill_pid() on its own, so setting null here should be safe */
#define KILL_TIMEOUT_CHILD 3

/* the global struct which holds the minicron config */
struct minicron_config{
	char *pidfile; /* malloc(3)-ed in parse_args, free(3)-ed in clean_config */
	unsigned int kill_after;
	unsigned int interval;
	char *child; /* malloc(3)-ed in parse_args, free(3)-ed in clean_config */
	char **argv; /* terminated with null pointer */
} config;

void usage(char *);
void init_config();
void clean_config();
int parse_args(int, char**);
void kill_pid(pid_t, unsigned int);
void daemonize();
void mainloop_sigtermhandler();
int mainloop();
void supervisor_sigchldhandler();
void supervisor_sigtermhandler();
void createpid(pid_t);
void deletepid();
int supervisor();
int child();

/* we keep the PIDs global to improve the communication with the signal handlers */
pid_t pid_child, pid_supervisor;

extern char **environ;

int main(int argc, char **argv) {
	int retval;
	
	init_config();
	if ((retval = parse_args(argc, argv))) {
		usage(argv[0]);
		return retval;
	}
	
	atexit(clean_config); /* the parent will die in mainloop(), so we should ensure proper cleanup */
	
	mainloop();
	
	/* unreachable */
	return 0;
}

void usage(char *progname) {
	fprintf(stderr, "usage: %s [-p<pidfile>] [-kN] interval child\n", progname);
}

void init_config() {
	config.pidfile = NULL;
	config.kill_after = 0;
	config.interval = 0;
	config.child = NULL;
	config.argv = NULL;
}

void clean_config() {
	if (config.pidfile != NULL) {
		free(config.pidfile);
		config.pidfile = NULL;
	}
	if (config.child != NULL) {
		free(config.child);
		config.child = NULL;
	}
}

int parse_args(int argc, char **argv) {
	int i;
	if (argc < 3)
		return 11;
		
	i = 1;
	while (argv[i][0] == '-') {
		argv[i]++;
		switch (argv[i][0]) {
			case 'p':
				argv[i]++;
				config.pidfile = malloc(sizeof(char) * strlen(argv[i]));
				memcpy(config.pidfile, argv[i], strlen(argv[i]));
				break;
			case 'k':
				argv[i]++;
				config.kill_after = atoi(argv[i]);
				break;
			default:
				return 13;
		}
		i++;
	}
	
	config.interval = (unsigned int) atoi(argv[i]);
	i++;
	
	config.child = malloc(sizeof(char) * strlen(argv[i]));
	memcpy(config.child, argv[i], strlen(argv[i]));
	
	/* 
	   the remaining arguments will later be passed to the child
	   note that config.argv[0] should be equal to config.child prior to execve(2), according to POSIX
	   that's why we don't increment the index after the last operation 
	*/
	if(argv[i] != NULL)
		config.argv = &argv[i];
	
	return 0;
}

void kill_pid(pid_t pid, unsigned int timeout) {
	int state = 0, waitpid_r = 0;
	
	waitpid_r = waitpid(pid, &state, WNOHANG); /* check the child state */
	
	if(!(WIFEXITED(state) || WIFSIGNALED(state)) || waitpid_r==0) /* the child has not exited yet */
		kill(pid, SIGTERM); /* sending SIGTERM to child */
		
	else 
		return;
	
	if (timeout>0) {
	/* the child may ignore the SIGTERM, so we wait and check again */
		waitpid_r = waitpid(pid, &state, WNOHANG);
		if(!(WIFEXITED(state) || WIFSIGNALED(state)) || waitpid_r==0) /* check again before sleeping, in order to avoid useless blocking */
			sleep(timeout); 
		else 
			return;

		waitpid_r = waitpid(pid, &state, WNOHANG);
	
		if(!(WIFEXITED(state) || WIFSIGNALED(state)) || waitpid_r==0)
			kill(pid, SIGKILL); /* finally send SIGKILL */
		else
			return;
	} else /* timeout == 0, wait the process to die */
		waitpid(pid, &state, 0);
}

void daemonize() {
	pid_t pid; int fd;
	
	if (getppid()==1) return; /* already daemonized */
	
	umask(027); /* set a restrictive umask */
		
	pid = fork();
	if (pid<0) exit(-1); /* fork error */
	else if (pid>0) exit(0); /* the parent should exit */
	
	setsid(); /* create a new session */
	
	/* ignoring the tty signals */
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	
	/* we don't need SIGCHLD yet */
	signal(SIGCHLD, SIG_IGN);
		
	/* close all fds */
	for (fd=0; fd<getdtablesize(); fd++)
		close(fd);
		
	/* reopen the basic fds and redirect them to /dev/null */
	fd = open("/dev/null", O_RDWR);
	dup2(fd, 0); /* stdin */
	dup2(fd, 1); /* stdout */
	dup2(fd, 2); /* stderr */
}

void mainloop_sigtermhandler() {
	kill_pid(pid_supervisor, KILL_TIMEOUT_SUPERVISOR);
	exit(1);
}

int mainloop() {
	signal(SIGTERM, mainloop_sigtermhandler);
	signal(SIGINT, SIG_IGN); /* ignoring SIGINT */
	while (1) {
		pid_supervisor = fork();
		if (pid_supervisor < 0) /* fork failed */
			continue;
		else if (pid_supervisor == 0)
			supervisor();

		sleep(config.interval);
		
		kill_pid(pid_supervisor, KILL_TIMEOUT_SUPERVISOR);
	}
}

void supervisor_sigchldhandler() {
	deletepid();
	_exit(0);
}

void supervisor_sigtermhandler() {
	kill_pid(pid_child, KILL_TIMEOUT_CHILD);
	deletepid();
	_exit(1);
}

void createpid(pid_t pid) {
	if (config.pidfile == NULL)
		return;
	FILE *pidfd;
	pidfd = fopen(config.pidfile, "w");
	fprintf(pidfd, "%d\n", pid);
	fclose(pidfd);
}

void deletepid() {
	if (config.pidfile == NULL)
		return;
	unlink(config.pidfile);
}

int supervisor() {
	signal(SIGTERM, supervisor_sigtermhandler); /* catch SIGTERM from the parent, if the interval has passed */
	signal(SIGCHLD, supervisor_sigchldhandler); /* catch SIGCHLD in order to _exit(2) immediately after the child returns */
	
	pid_child = fork();
	if (pid_child < 0) /* fork failed */
		_exit(-1);
	else if (pid_child == 0)
		child();
		
	createpid(pid_child);
	// atexit(deletepid); /* won't work if the supervisor is killed by a signal! */
		
	if (config.kill_after) {
		sleep(config.kill_after);
		kill_pid(pid_child, KILL_TIMEOUT_CHILD);
	} else
		wait(0);

	
	deletepid();
		
	_exit(0);
}

int child() {
	execve(config.child, config.argv, environ);
	/* execve(2) returns only on error, so if we reached this point, something is not OK */
	_exit(-1); 
}
