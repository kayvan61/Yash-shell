#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>

extern char** environ;

typedef struct {
    int isBackground;
    pid_t processGroupID;
    pid_t processID;
    int stdFds[3];
    char* cmd;
    char** argv;
    int isDone;
    int isStopped;
    char* fullCommandStr;
    int num;
} Job;

Job* jobList[200];
int jobArrIndex = 0;
int jobNumber = 1;
Job* activeJob;

// tokenizes a string using " " as a delimiter
// return is allocated on the heap and must be freed by the caller
//
// input : char*  inputstring : the string to tokenize
//         char*  tokenString : string containing all the delimiters for strTok
//         int *  numRef      : contains the size of the returned array (can be NULL)
// output: char** resultArray : a 2d array of tokens on the heap
char** tokenizeInput(char *inputString, int* numRef, char* tokenString);

// finds a specific token an returns its index in the array of tokens
// assumes that the array is null terminated
//
// input  : const char ** tokArr : null terminated array of tokens
//          const char *  tok    : token to search for null term
// output : int                  : index of the token or -1 if not found
int findTok(const char** tokArr, const char* tok);

void execJob(const Job* jobToExec);
Job* createJob(char** tokenList, const pid_t pgid, const char* cmd, const int pipeRead, const int pipeWrite, const char*);
void updateChildrenStatus();
void freeNulled2d(char**);
void printJobs();
void doFg();
void doBg();
void checkJobStatus(Job* jobToCheck, int Flags);
void cleanZombies();
int getHighestJobNum();

Job* findActiveJob() {
    for(int i = jobArrIndex-1; i >= 0; i--) {
        if(!jobList[i]->isBackground && !jobList[i]->isDone) {
            return jobList[i];
        }
    }
    return NULL;
}

void sigtstp_handler(int signo)
{
    Job* activeJob = findActiveJob();
    if(activeJob == NULL) {
        return;
    }
    killpg( activeJob->processGroupID, SIGTSTP );    
    wait((int*) NULL);
    activeJob->isBackground = 1;
    tcsetpgrp(STDIN_FILENO, getpgid(getpid()));
    tcsetpgrp(STDOUT_FILENO, getpgid(getpid()));
}

void sigint_handler(int signo)
{
    printf("sigCHLD handler got signo: %d\n", signo);
    //tcsetpgrp(STDIN_FILENO, getpgid(getpid()));
    //tcsetpgrp(STDOUT_FILENO, getpgid(getpid()));
}

void sigchld_handler(int signo) {
#ifdef DEBUG
    printf("sigCHLD handler got signo: %d\n", signo);
#endif
    updateChildrenStatus();
    cleanZombies();
}

int main(int argc, char** argv) {

    char* prompt = "# ";
    char* userCmd = NULL;
    char* cmdStored = NULL;
    char** procsIn = NULL;
    char** tokIn = NULL;
    Job* newJob = NULL;
    pid_t childPID = -1;
    int pipeFD[2];    

    signal(SIGINT, sigint_handler);
    //    signal(SIGTSTP, sigtstp_handler);
    signal(SIGCHLD, sigchld_handler);
    
    setpgid(0,0);
    
    while((userCmd = readline(prompt))) {       
        // check if we have any pipes (need to spawn more than one process)
        cmdStored = malloc((strlen(userCmd) + 1) * sizeof(char));
        strcpy(cmdStored, userCmd);
        int numProcs = 0;
        procsIn = tokenizeInput(userCmd, &numProcs, "|");

        if(strcmp(userCmd, "jobs") == 0) {
            printJobs();
            continue;
        }

        if(strcmp(userCmd, "fg") == 0) {
            doFg();
            continue;
        }

        if(strcmp(userCmd, "bg") == 0) {
            doBg();
            continue;
        }

        if (numProcs > 2 || procsIn == NULL) {
            continue;
        }
        else if (numProcs == 2) {
            // create the pipe
            if(pipe(pipeFD)) {
                printf("Making of the pipe failed. check errno for the reason.\n");
                exit(-1);
            }                       

            tokIn = tokenizeInput(userCmd, NULL, " ");
            pid_t procGID = -1;
            childPID = fork();
            procGID = childPID;
            newJob = createJob(tokIn, childPID, tokIn[0], -1, pipeFD[1], NULL);
            if(newJob == NULL) {
                if(childPID == 0) {
                    return 0;
                }
                else {
                    continue;
                }
            }
            if(childPID == 0) {
                close(pipeFD[0]);
                execJob(newJob);
            }
            else {
                // add Job to job list
                newJob->processID = childPID;
                newJob->num = getHighestJobNum()+1;
                close(newJob->stdFds[STDIN_FILENO]);
                close(newJob->stdFds[STDOUT_FILENO]);
                close(newJob->stdFds[STDERR_FILENO]);
            }                                    

            childPID = fork();
            tokIn = tokenizeInput(procsIn[1], NULL, " ");
            newJob = createJob(tokIn, procGID, tokIn[0], pipeFD[0], -1, cmdStored);
            if(newJob == NULL) {
                if(childPID == 0) {
                    return 0;
                }
                else {
                    continue;
                }
            }
            if (childPID == 0) {
                close(pipeFD[1]);
                execJob(newJob);
            }
            else {
                newJob->processID = childPID;
                newJob->num = getHighestJobNum()+1;
                jobList[jobArrIndex++] = newJob;
                close(newJob->stdFds[STDIN_FILENO]);
                close(newJob->stdFds[STDOUT_FILENO]);
                close(newJob->stdFds[STDERR_FILENO]);
            }            
            close(pipeFD[0]);
            close(pipeFD[1]);            
        }
        else {           
            tokIn = tokenizeInput(userCmd, NULL, " ");
            childPID = fork();
            newJob = createJob(tokIn, childPID, tokIn[0], -1, -1, cmdStored);
            
            if(newJob == NULL) {
                if(childPID == 0) {
                    return 0;
                }
                else {
                    continue;
                }
            }
            if(childPID == 0) {
                execJob(newJob);
            }
            else {
                // add Job to job list
                newJob->processID = childPID;
                newJob->num = getHighestJobNum()+1;
                jobList[jobArrIndex++] = newJob;
                close(newJob->stdFds[STDIN_FILENO]);
                close(newJob->stdFds[STDOUT_FILENO]);
                close(newJob->stdFds[STDERR_FILENO]);
            }
        }
                
        activeJob = findActiveJob();
        if(activeJob != NULL) {
            tcsetpgrp(0, activeJob->processGroupID);
            activeJob -> isBackground = 0;
#ifdef DEBUG
            printf("control given to process Group: %d\n", activeJob->processGroupID);
#endif
            checkJobStatus(activeJob, WEXITED | WSTOPPED | WCONTINUED);
            tcsetpgrp(0, getpgid(getpid()));
        }        

        
        // clean up our mess
        if (tokIn != NULL) {
            freeNulled2d(tokIn);
            free(tokIn);
            tokIn = NULL;
        }
        
        if (procsIn != NULL) {
            freeNulled2d(procsIn);
            free(procsIn);
            procsIn = NULL;
        }
        free(userCmd);
        updateChildrenStatus();
    }
}

void freeNulled2d(char** arrToFree) {
    for(int i = 0; arrToFree[i] != NULL; i++){
        free(arrToFree[i]);        
    }
}

void doFg() {
    if(jobArrIndex <= 0) {
        #ifdef DEBUG
        printf("No Jobs to FG!");
        #endif 
        return;
    }
    
    Job* activeJob = jobList[jobArrIndex-1];
    if(activeJob != NULL) {
        killpg(activeJob->processGroupID, SIGCONT);
        checkJobStatus(activeJob, WEXITED | WSTOPPED | WCONTINUED);
        activeJob->isBackground = 0;
        activeJob->isStopped = 0;
        activeJob->isDone = 0;
        tcsetpgrp(0, activeJob->processGroupID);
        #ifdef DEBUG
        printf("control given to process Group: %d\n", activeJob->processGroupID);
        #endif
        checkJobStatus(activeJob, WEXITED | WSTOPPED | WCONTINUED);

        tcsetpgrp(0, getpgid(getpid()));
    }        
    updateChildrenStatus();
}

void doBg() {
    Job* activeJob = jobList[jobArrIndex-1];
    activeJob->isStopped = 0;
    activeJob->isDone = 0;
    activeJob->isBackground = 1;
    killpg(activeJob->processGroupID, SIGCONT);
}

void checkJobStatus(Job* jobToCheck, int flags) {
    if(jobToCheck->isDone == 1 && 
       jobToCheck->isBackground == 1) {
        return;
    }
    siginfo_t status;
    status.si_pid = 0;
    waitid(P_PID, jobToCheck->processID,
           &status,
           flags);
    if(status.si_pid != 0) {
        if (status.si_code == SIGCONT ||
            status.si_code == CLD_CONTINUED) {
                #ifdef DEBUG
            printf("process <%d> was continued\n", jobToCheck->processID);
                #endif
            jobToCheck->isDone = 0;
            jobToCheck->isStopped = 0;
        }
        else if (status.si_code == SIGTSTP ||
                 status.si_code == SIGSTOP ||
                 status.si_code == CLD_STOPPED) {
                #ifdef DEBUG
            printf("process <%d> was stopped\n", jobToCheck->processID);
                #endif
            jobToCheck->isDone = 0;
            jobToCheck->isStopped = 1;
            jobToCheck->isBackground = 1;
        }
        else if(status.si_code == SIGINT ||
                status.si_code == SIGHUP ||
                status.si_code == SIGKILL ||
                status.si_code == SIGTERM ||
                status.si_code == CLD_KILLED ||
                status.si_code == CLD_EXITED) {
                #ifdef DEBUG
            printf("process <%d> was killed\n", jobToCheck->processID);
                #endif
            if(jobToCheck->isBackground == 1) {
                printf("[%d] + %s       %s\n",
                       jobToCheck->num,
                       "Done",
                       jobToCheck->fullCommandStr);
            }
            jobToCheck->isDone = 1;
            jobToCheck->isBackground = 1;
            jobToCheck->isStopped = 1;
        }
        else{
                #ifdef DEBUG
            printf("process with PID: %d got status : %d\n",status.si_pid, status.si_code);
            printf("for reference CLD_CONTINUED=%d, CLD_STOPPED=%d, CLD_EXITED=%d", CLD_CONTINUED, CLD_STOPPED, CLD_EXITED);
                #endif
        }       
    }
}

int getHighestJobNum() {
    int ret = 0;
    for(int i = 0; i < jobArrIndex; i++) {
        if(jobList[i]->num > ret) {
            ret = jobList[i]->num;
        }
    }
    return ret;
}

void cleanZombies() {
    for(int i = 0; i < jobArrIndex; i++) {
        checkJobStatus(jobList[i], WEXITED | WSTOPPED | WCONTINUED | WNOHANG);
    }
}

void updateChildrenStatus() {    
    for(int i = 0; i < jobArrIndex; i++) {

        checkJobStatus(jobList[i], WEXITED | WSTOPPED | WCONTINUED | WNOHANG);
        
        if(jobList[i]->isBackground && jobList[i]->isDone && jobList[i]->isStopped) {
            waitpid(jobList[i]->processID, (int*)NULL, WNOHANG);
            for(int j = i; j < jobArrIndex; j++){
                jobList[j] = jobList[j+1];
            }
            i--;
            jobArrIndex--;
        }
    }
}

void printJobs() {    
    if(jobArrIndex == 0) {
        return;
    }
    for(int i = 0; i < jobArrIndex-1; i++) {
        printf("[%d] - %s       %s\n",
               jobList[i]->num,
               jobList[i]->isStopped ? "Stopped" : "Running",
               jobList[i]->fullCommandStr);
    }
    printf("[%d] + %s       %s\n",
           jobList[jobArrIndex-1]->num,
           jobList[jobArrIndex-1]->isStopped ? "Stopped" : "Running",
           jobList[jobArrIndex-1]->fullCommandStr);
}

void execJob(const Job* toExec) {

    if(toExec->stdFds[STDIN_FILENO] != -1) {
        dup2(toExec->stdFds[STDIN_FILENO], STDIN_FILENO);
    }

    if(toExec->stdFds[STDOUT_FILENO] != -1) {
        dup2(toExec->stdFds[STDOUT_FILENO], STDOUT_FILENO);
    }

    if(toExec->stdFds[STDERR_FILENO] != -1) {
        dup2(toExec->stdFds[STDERR_FILENO], STDERR_FILENO);
    }
    
    if(toExec->processGroupID <= 0) {
        setpgid(0,0);
    }
    else {
        setpgid(0, toExec->processGroupID);
    }

    signal(SIGTTOU, SIG_DFL);                 // ignore these signals
    signal(SIGTTIN, SIG_DFL);                 // ignore these signals
    signal(SIGINT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);
    
    if(execvpe(toExec->cmd, toExec->argv, environ)) {
        exit(-1);
    }    
}

/*
    int isBackground;
    pid_t processGroupID;
    int[3] stdFds;
    char* cmd;
    char** argv;
 */
Job* createJob(char** tokenList, const pid_t pgid, const char* cmd, const int pipeRead, const int pipeWrite, const char* callingStr) {

    Job* ret = malloc(sizeof(Job));

    if(callingStr != NULL) {
        ret->fullCommandStr = malloc(sizeof(char) * (strlen(callingStr) + 1));
        strcpy(ret->fullCommandStr, callingStr);
    } else {
        ret->fullCommandStr = NULL;
    }
    
    // input redir
    unsigned int indexOfOutputRedir = findTok((const char**)tokenList, "<");
    if(indexOfOutputRedir != -1){
        int inputFD = open(tokenList[indexOfOutputRedir + 1], O_RDONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        if(inputFD < 0){
            #ifdef DEBUG            
            printf("error opening file for input redirection. code: %d\n", errno);
            #endif
            return NULL;
        }
        ret->stdFds[STDIN_FILENO] = inputFD;
    }
    else if(pipeRead != -1) {
        ret->stdFds[STDIN_FILENO] = pipeRead;
    }
    else {
        ret->stdFds[STDIN_FILENO] = -1;
    }

    // output redir
    unsigned int indexOfInputRedir = findTok((const char**)tokenList, ">");
    if(indexOfInputRedir != -1){
        int outputFD = open(tokenList[indexOfInputRedir + 1], O_CREAT | O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        if(outputFD < 0){
            #ifdef DEBUG
            printf("error opening file for output redirection. code: %d\n", errno);
            #endif
            return NULL;
        }
        ret->stdFds[STDOUT_FILENO] = outputFD;
    }
    else if(pipeWrite != -1) {
        ret->stdFds[STDOUT_FILENO] = pipeWrite;
    }
    else {
        ret->stdFds[STDOUT_FILENO] = -1;
    }

    // stderr redir
    unsigned int indexOfErrRedir = findTok((const char**)tokenList, "2>");
    if(indexOfErrRedir != -1){
        int errFD = open(tokenList[indexOfErrRedir + 1], O_CREAT | O_WRONLY, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
        if(errFD < 0){
            #ifdef DEBUG
            printf("error opening file for error redirection. code: %d\n", errno);
            #endif
            return NULL;
        }
        ret->stdFds[STDERR_FILENO] = errFD;
    }
    else {
        ret->stdFds[STDERR_FILENO] = -1;
    }
    
    int lengthOfTokenList = 0;
    while(tokenList[lengthOfTokenList++]);

    int isBack = findTok((const char**)tokenList, "&");
    if(isBack != lengthOfTokenList-2 && isBack != -1) {
        return NULL;
    }
    ret->isBackground = isBack == -1 ? 0 : 1;

    if(pgid <= 0) {
        ret->processGroupID = getpid();
    }
    else{
        ret->processGroupID = pgid;
    }
    ret->processID      = getpid();

    ret->cmd = (char*)malloc(sizeof(char) * (strlen(cmd) + 1));
    strcpy(ret->cmd, cmd);

    // make sure the end of the argument list ends with NULL
    int indexOfAmp = findTok((const char**)tokenList, "&");
    int indexOfNull = -1;
    if(indexOfAmp != -1) {
        indexOfNull = indexOfAmp;
    }
    if(indexOfErrRedir != -1) {
        indexOfNull = indexOfNull < indexOfErrRedir ? indexOfNull : indexOfErrRedir; 
    }
    if(indexOfOutputRedir != -1) {
        indexOfNull = indexOfNull < indexOfOutputRedir ? indexOfNull : indexOfOutputRedir; 
    }
    if(indexOfInputRedir != -1) {
        indexOfNull = indexOfNull < indexOfInputRedir ? indexOfNull : indexOfInputRedir; 
    }
    
    if(indexOfNull != -1) {
        tokenList[indexOfNull] = NULL;
    }
    
    ret->argv = (char**)malloc(sizeof(char*) * (lengthOfTokenList + 1));

    ret->isStopped = 0;
    ret->isDone = 0;    
    
    int index = 0;
    while(tokenList[index]) {
        ret->argv[index] = malloc(sizeof(char) * (strlen(tokenList[index]) + 1));
        strcpy(ret->argv[index], tokenList[index]);
        index++;
    }
    ret->argv[index] = NULL;
    return ret;
}

char** tokenizeInput(char *inputString, int* numRef, char* tokenString) {

    if(*inputString == 0){
        return NULL;
    }
    
    char **resultArr = (char**)malloc(1*sizeof(char*));
    int resultArrIndex = 0;
    int resultArrSize = 1;
    char *currentToken = NULL;
    
    currentToken = strtok(inputString, tokenString);
    
    do {
        resultArr[resultArrIndex] = (char *)malloc((strlen(currentToken) + 1) * sizeof(char));
        strcpy(resultArr[resultArrIndex], currentToken);
        resultArrIndex += 1;
        if(resultArrIndex == resultArrSize){
            // resize result array
            resultArrSize *= 2;
            resultArr = (char **)realloc(resultArr, resultArrSize * sizeof(char*));
        }
    } while((currentToken = strtok(NULL, tokenString)));

    if(resultArrIndex >= resultArrSize) {
        resultArr = (char **)realloc(resultArr, (resultArrSize+1) * sizeof(char*));
    }
    resultArr[resultArrIndex] = NULL;

    if(numRef != NULL) {
        *numRef = resultArrIndex;
    }
    return resultArr;
}

int findTok(const char** tokArr, const char* tok) {
    int index = 0;
    while(tokArr[index] != NULL) {
        if(strcmp(tokArr[index], tok) == 0){
            return index;
        }
        index++;        
    }
    return -1;
}
