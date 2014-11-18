/*
 * -- simplehttpd.c --
 * A (very) simple HTTP server
 *
 * Sistemas Operativos 2014/2015
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/shm.h>
#include <fcntl.h>

// Produce debug information
#define DEBUG	  	0

// Header of HTTP reply to client
#define	SERVER_STRING 	"Server: simpleserver/0.1.0\r\n"
#define HEADER_1	"HTTP/1.0 200 OK\r\n"
#define HEADER_2	"Content-Type: text/html\r\n\r\n"

#define GET_EXPR	"GET /"
#define CGI_EXPR	"cgi-bin/"
#define SIZE_BUF	1024


#define TRUE 1
#define FALSE 0
#define STATIC 2
#define DYNAMIC 3
#define FIFO 4

typedef struct configuration{
    int server_port;
    int n_threads;
    int scheduling_policy;
    char scripts_list[5][10];//5 scripts authorized
}Configuration;

typedef struct thread_pool
{
    pthread_t mythread;
    sem_t *sem;
    int occupied;
}thread_pool;

typedef struct request
{
    int type;
    char file[SIZE_BUF];
    int client_socket;
    int dispatched;
}Request;

int  fireup(int port);
void identify(int socket);
void get_request(int socket);
int  read_line(int socket, int n);
void send_header(int socket);
void send_page(Request temp);
void execute_script(Request temp);
void not_found(int socket);
void catch_ctrlc(int);
void cannot_execute(int socket);
void init();
void configuration_manager();
void statistics_manager();
void *scheduler();
void *dispatchRequests(void *id);
void buffer_full(int socket);
int is_valid(Request temp);


char buf[SIZE_BUF];
char req_buf[SIZE_BUF];
char buf_tmp[SIZE_BUF];
int port,socket_conn,new_conn;

int scheduling_policy= FIFO;

int *id_threads,read_pos = 0,write_pos = 0;
pid_t childs[2];
pthread_t scheduler_thread;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

sem_t *stop_scheduler;
Request *requests;//the buffer
thread_pool *pool;
sem_t *stop_scheduler;

int semid, shmid;
Configuration *shared_mem;

int main(int argc, char ** argv)
{
    struct sockaddr_in client_name;
    socklen_t client_name_len = sizeof(client_name);
    int port,i,found_slot;
    // Verify number of arguments
    ////////////////create shared memory
    shmid = shmget(IPC_PRIVATE,sizeof(Configuration),IPC_CREAT|0777);
    shared_mem = shmat(shmid,NULL,0);
    ///////////////////////////////
    //load files to shared memory
    char buffer[100];
    char *str;
    FILE *fp=fopen("serverconfigs.txt","r+");
    if(fp == NULL)
    {
        printf("Error opening serverconfigs.\n");
        exit(-1);
    }
    else
    {
        fscanf(fp,"%s",buffer); //read initial numbers
        str = strtok(buffer,";");
        shared_mem->server_port=atoi(str);
        str = strtok(NULL,";");
        shared_mem->n_threads=atoi(str);
        str = strtok(NULL,";");
        shared_mem->scheduling_policy=atoi(str);
        int i=0;
        while(fscanf(fp,"%s",buffer) != EOF)//read list os scripts;
        {
            strcpy(shared_mem->scripts_list[i],buffer);
            i++;
        }
        fclose(fp);
    }
    //////////////////////////////////PONTEIROS
    int ids[shared_mem->n_threads];
    id_threads=ids;
    
    Request pedidos[shared_mem->n_threads*2];
    requests=pedidos;
    
    port=shared_mem->server_port;
    
    thread_pool piscina[shared_mem->n_threads];
    pool=piscina;
    ///////////////////////////////////////
    // Configure listening port
    if ((socket_conn=fireup(port))==-1)
        exit(1);
    // initialize processes and threads
    init();
        // catch ^C
    signal(SIGINT,catch_ctrlc);
    // Serve requests
    
    printf("INITIAL CONFIGS\nport=%d\nnumber-threads=%d\npolicy=%d\n\n",shared_mem->server_port, shared_mem->n_threads,shared_mem->scheduling_policy);
    while (1)
    {
       
        found_slot = FALSE;
        // Accept connection on socket
        if ((new_conn = accept(socket_conn,(struct sockaddr *)&client_name,&client_name_len)) == -1 )
        {
            printf("Error accepting connection\n");
            exit(1);
        }
        // search for a space in requests buffer and update write_pos
        #if DEBUG
        printf("Searching for an available slot in the requests buffer.\n");
        #endif
        
        printf("TESTE\n");
        
        for(i=0;i<shared_mem->n_threads*2;i++)
        {
            //printf("\ni=%d, dispatched=%d\n",i,requests[i].dispatched);
            if(requests[i].dispatched == TRUE)//se for um tem espaço
            {
                found_slot = TRUE;
                write_pos = i;
                break;
            }
        }
        if(found_slot == FALSE)
        {
            printf(">>>>>>>>>>>>>>>>>>AQUI <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n");
            buffer_full(new_conn);
            continue;
        }
        identify(new_conn);
        
        // Identify new client
        // Process request
        get_request(new_conn);
        // Verify if request is for a page or script
        if(!strncmp(req_buf,CGI_EXPR,strlen(CGI_EXPR)))
            requests[write_pos].type = DYNAMIC;
        else
            requests[write_pos].type = STATIC;
        #if DEBUG
        printf("Adding a new request to the requests buffer.\n");
        #endif
        // set the request to undispatched
        requests[write_pos].dispatched = FALSE;
        // associate the new established connection to the request
        requests[write_pos].client_socket = new_conn;
        // associate the file requested to the request
        strcpy(requests[write_pos].file,"");
        strcpy(requests[write_pos].file,req_buf);
        // awake the scheaduler thread to search which is the next available thread and the next request to be dispatched
#if DEBUG
        printf("POSTING THE SCHEDULER THREADS.\n");
#endif
        sem_post(stop_scheduler);
        printf("GOING AROUND.\n");
        
    }
    
    
    // wait for threads in the thread pool
    for(i=0;i<shared_mem->n_threads;i++)
    {
        pthread_join(pool[i].mythread,NULL);
    }
    // wait for threads in the scheduler
    pthread_join(scheduler_thread,NULL);
    // wait for other processes
    while(wait(NULL) !=-1);
    return 0;
}


// initialize processes and threads

void init()
{
    
    int i;
    char aux[30];
    #if DEBUG
    printf("Initializing the necessary resources.\n");
    #endif
    // open scheduler semaphore
    sem_unlink("STOP_SCHEDULER");
    stop_scheduler = sem_open("STOP_SCHEDULER",O_CREAT|O_EXCL,0700,0);
    // set the initial requests to dispatched, to say to the dispatch function that there is no requests yet
    for(i=0;i<(shared_mem->n_threads*2);i++)
    {
        requests[i].dispatched = TRUE;
    }
    // initialize semaphores from thread pool
    for(i=0;i<(shared_mem->n_threads);i++)
    {
        id_threads[i] = i;
        pool[i].occupied = FALSE;
        sprintf(aux,"SEM_%d",i);
        sem_unlink(aux);
        pool[i].sem = sem_open(aux,O_CREAT|O_EXCL,0700,0);
    }
    //create configuration manager process and statistics manager process
   /* if( (childs[0] = fork()) ==0)
    {
        configuration_manager();
        exit(0);
    }
    if((childs[1] = fork()) ==0)
    {
        statistics_manager();
        exit(0);
    }*/
    // create scheduler thread
    
    if(pthread_create(&scheduler_thread,NULL,scheduler,NULL)!=0)
    {
        perror("\nError creating scheduler thread !.\n");
    }
    // create thread pool
    for(i=0;i<shared_mem->n_threads;i++)
    {
        if(pthread_create(&(pool[i].mythread),NULL,dispatchRequests,&id_threads[i])!=0)
        {
            perror("\nError creating dispatch thread !\n");
        }
    }
    return;
}



// configuration manager process
void configuration_manager()
{
    while(1)
    {
        printf("\nConf manager\n");
        sleep(25);
    }
}


// statistics manager process
void statistics_manager()
{
    while(1)
    {
        printf("\nstatistics_manager\n");
        sleep(25);
    }
}


// scheduler thread - decides which is the next thread and which is the next request to be dispatched----------------------------------------- NAO FOI TESTADO SE FUNCIONA OU NAO--------------------------------
void *scheduler()
{
    int i,found_thread,found_request;
    while(1)
    {
        sem_wait(stop_scheduler);
        found_thread = FALSE;
        found_request = FALSE;
        // search the next request to be dispatched(FIFO, priority to static requests, priority to dynamic requests), it has to be valid
        // FIFO policy
#if DEBUG
        printf("Searching the next request to be dispatched.\n");
#endif
        if(scheduling_policy == FIFO)
        {
            for(i=read_pos;i<(shared_mem->n_threads*2) + read_pos;i++)
            {
                if(requests[i%(shared_mem->n_threads*2)].dispatched == FALSE && is_valid(requests[i%(shared_mem->n_threads*2)])== TRUE)
                {
                    found_request = TRUE;
                    read_pos = i%(shared_mem->n_threads*2);
                    break;
                }
                else
                {
                    requests[i%(shared_mem->n_threads*2)].dispatched = TRUE;
                }
            }
        }
        ///////////////////
        /*if(scheduling_policy == FIFO)
        {
            for(i=read_pos;i<shared_mem->n_threads*2;i++)
            {
                if(requests[i].dispatched == FALSE && is_valid(requests[i])== TRUE)
                {
                    found_request = TRUE;
                    read_pos = i;
                    break;}
                else
                {    requests[i].dispatched = TRUE;}
            }
        }*/
        else if(scheduling_policy == STATIC) 		// priority to static requests
        {
           
            for(i=read_pos;i<(shared_mem->n_threads*2) + read_pos;i++)
            {
                if(requests[i%(shared_mem->n_threads*2)].dispatched == FALSE && requests[i%(shared_mem->n_threads*2)].type == STATIC)
                {
                    if(is_valid(requests[i%(shared_mem->n_threads*2)]) == TRUE)
                    {
                        found_request = TRUE;
                        read_pos = i%(shared_mem->n_threads*2);
                        break;
                    }
                    else
                    {
                        requests[i%(shared_mem->n_threads*2)].dispatched = TRUE;
                    }
                }
            }
            if(found_request == FALSE)				// if there is no static requests, search for one of the other type
            {
                for(i=0;i<shared_mem->n_threads*2;i++)
                {
                    if(requests[i].dispatched == FALSE)
                    {
                        if(is_valid(requests[i]) == TRUE)
                        {
                            found_request = TRUE;
                            read_pos = i;
                            break;
                        }
                        else
                        {
                            requests[i].dispatched = TRUE;
                        }
                    }
                }
            }
        }
        else 										// priority to dynamic requests
        {
            for(i=read_pos;i<(shared_mem->n_threads*2)+read_pos;i++)
            {
                if(requests[i%(shared_mem->n_threads*2)].dispatched == FALSE && requests[i%(shared_mem->n_threads*2)].type == DYNAMIC)
                {
                    if(is_valid(requests[i%(shared_mem->n_threads*2)]) == TRUE)
                    {
                        found_request = TRUE;
                        read_pos = i%(shared_mem->n_threads*2);
                        break;
                    }
                    else
                    {
                        requests[i%(shared_mem->n_threads*2)].dispatched = TRUE;
                    }
                }
            }
            if(found_request == FALSE)					// if there is no dynamic requests, search for one of the other type
            {
                for(i=0;i<shared_mem->n_threads*2;i++)
                {
                    if(requests[i].dispatched == FALSE)
                    {
                        if(is_valid(requests[i]) == TRUE)
                        {
                            found_request = TRUE;
                            read_pos = i;
                            break;
                        }
                        else
                        {
                            requests[i].dispatched = TRUE;
                        }
                    }
                }
            }
        }
        // if none of the undispatched requests is valid, do not call a thread
        if(found_request == FALSE)
        {
#if DEBUG
            printf("None of the undispatched requests is valid.\n");
#endif
            continue;
        }
        // search the next thread to execute
#if DEBUG
        printf("Searching the next thread to execute.\n");
#endif
        while(found_thread == FALSE)
        {
            
            for(i=0;i<shared_mem->n_threads;i++)
            {
                if(pool[i].occupied  == FALSE)
                {
                    printf("\nSignal thread number=%d\n",i);
                    found_thread = TRUE;
                    sem_post(pool[i].sem);
                    
                    break;
                }
            }
            
            if(found_thread == FALSE)
            {
#if DEBUG
                printf("All threads are busy, waiting...\n");
#endif
                // just in case, if all the threads are busy, waits until one thread is waiting to execute
                //sleep(2);
            }
        }
        
        
    }
}



// dispatch requests
void *dispatchRequests(void *id)
{
    
    int my_id = *((int*) id);
    int old_read_pos;
    //sem_wait(pool[my_id].sem);
    while(1)
    {
        // thread waits for its time to execute
        sem_wait(pool[my_id].sem);
        // mutual exclusion zone
        pthread_mutex_lock(&mutex);
        old_read_pos = read_pos;
        pool[my_id].occupied = TRUE;
        //printf("Thread %d will dispatch one request.\n",my_id);
        pthread_mutex_unlock(&mutex);
        //Verify if request is for a page or script
        if(requests[old_read_pos].type == DYNAMIC)
            execute_script(requests[old_read_pos]);
        else
            // Search file with html page and send to client
            send_page(requests[old_read_pos]);
        // Terminate connection with client
        close(requests[old_read_pos].client_socket);
        //this request is already dispatched
        printf("Thread %d vai dormir.\n",my_id);
        sleep(20);
        requests[old_read_pos].dispatched = TRUE;
        printf("Thread %d acordada.\n",my_id);
        // change the status of the thread to waiting for execute
        pool[my_id].occupied = FALSE;
        
#if DEBUG
        printf("Thread %d dispatched one request.\n",my_id);
#endif
    }
}



// scheduler verifies if a request is valid, verifies if a file exists or if a script has permission to run -------------- FALTA LER DO FICHEIRO DE CONFIGURAÇÃO SE TEM PERMISSAO OU NAO--------------------------------
int is_valid(Request temp)
{
#if DEBUG
    printf("Checking if a request is valid.\n");
#endif
    FILE * fp;
    char aux[SIZE_BUF];
    if(temp.type == STATIC)
    {
        // Searchs for page in directory htdocs
        sprintf(aux,"htdocs/%s",temp.file);
        // Verifies if file exists
        if((fp=fopen(aux,"rt"))==NULL)
        {
            // Page not found, send error to client
            printf("is_valid: page %s not found, alerting client\n",aux);
            not_found(temp.client_socket);
            close(temp.client_socket);
            return FALSE;
        }
        else
        {
            return TRUE;
        }
    }
    else
    {
        sprintf(aux,"scripts/%s",temp.file);
        if((fp=fopen(aux,"rt"))==NULL)
        {
            // Page not found, send error to client
            printf("is_valid: script %s not found, alerting client\n",aux);
            not_found(temp.client_socket);
            close(temp.client_socket);
            return FALSE;
        }
        else
        {
            return TRUE;
        }
    }
}



// warning users if the requests buffer is full
void buffer_full(int socket)
{
    
    char *erro ="error.html";
    FILE * fp;
    sprintf(buf_tmp,"htdocs/%s",erro);
    if((fp=fopen(buf_tmp,"rt"))==NULL) {
        // Page not found, send error to client
        printf("send_page: page %s not found, alerting client\n",buf_tmp);
        not_found(socket);
    }
    else {
                // First send HTTP header back to client
        send_header(socket);
        printf("send_page: sending page %s to client\n",buf_tmp);
        while(fgets(buf_tmp,SIZE_BUF,fp))
            send(socket,buf_tmp,strlen(buf_tmp),0);
        // Close file
        fclose(fp);
    }
    close(socket);
    return;
    
}

// Processes request from client
void get_request(int socket)
{
    sprintf(req_buf,"");
    int i,j;
    int found_get;
    found_get=0;
    while ( read_line(socket,SIZE_BUF) > 0 ) {
        if(!strncmp(buf,GET_EXPR,strlen(GET_EXPR))) {
            // GET received, extract the requested page/script
            found_get=1;
            i=strlen(GET_EXPR);
            j=0;
            while( (buf[i]!=' ') && (buf[i]!='\0') )
                req_buf[j++]=buf[i++];
            req_buf[j]='\0';
        }
    }
    // Currently only supports GET
    if(!found_get) {
        printf("Request from client without a GET\n");
        exit(0);
    }
    // If no particular page is requested then we consider htdocs/index.html
    if(!strlen(req_buf))
        sprintf(req_buf,"index.html");
//#if DEBUG
    printf("get_request: client requested the following page: %s\n",req_buf);
//#endif
    return;
}


// Send message header (before html page) to client
void send_header(int socket)
{
#if DEBUG
    printf("send_header: sending HTTP header to client\n");
#endif
    sprintf(buf,HEADER_1);
    send(socket,buf,strlen(HEADER_1),0);
    sprintf(buf,SERVER_STRING);
    send(socket,buf,strlen(SERVER_STRING),0);
    sprintf(buf,HEADER_2);
    send(socket,buf,strlen(HEADER_2),0);
    return;
}


// Execute script in /cgi-bin
void execute_script(Request temp)
{
    
    int fd[2],n=0;
    char aux[SIZE_BUF];
    char aux_buffer[SIZE_BUF];
    pid_t son;
    sprintf(aux,"scripts/%s",temp.file);
    if(pipe(fd))
    {
        printf("Error creating pipe !\n");
        cannot_execute(temp.client_socket);
    }
#if DEBUG
    printf("\nExecuting script : %s.\n", aux);
#endif
    if((son = fork())==0)
    {
        dup2(fd[1],fileno(stdout));
        close(fd[0]);
        close(fd[1]);
        execlp(aux,aux,NULL);
    }
    else
    {
        close(fd[1]);
        do
        {
            n = read(fd[0], aux_buffer, sizeof(aux_buffer));
            if(n>0)
            {
                aux_buffer[n] = '\0';
                send(temp.client_socket,aux_buffer,strlen(aux_buffer),0);
            }
        }while(n>0);
    }
    waitpid(son,NULL,0);
}


// Send html page to client
void send_page(Request temp)
{
    FILE * fp;
    // Searchs for page in directory htdocs
    sprintf(buf_tmp,"htdocs/%s",temp.file);
#if DEBUG
    printf("send_page: searching for %s\n",buf_tmp);
#endif
    // Verifies if file exists
    if((fp=fopen(buf_tmp,"rt"))==NULL) {
        // Page not found, send error to client
        printf("send_page: page %s not found, alerting client\n",buf_tmp);
        not_found(temp.client_socket);
    }
    else {
        // Page found, send to client
        // First send HTTP header back to client
        send_header(temp.client_socket);
        printf("send_page: sending page %s to client\n",buf_tmp);
        while(fgets(buf_tmp,SIZE_BUF,fp))
            send(temp.client_socket,buf_tmp,strlen(buf_tmp),0);
        // Close file
        fclose(fp);
    }
    return;
}


// Identifies client (address and port) from socket
void identify(int socket)
{
    char ipstr[INET6_ADDRSTRLEN];
    socklen_t len;
    struct sockaddr_in *s;
    int port;
    struct sockaddr_storage addr;
    len = sizeof addr;
    getpeername(socket, (struct sockaddr*)&addr, &len);
    // Assuming only IPv4
    s = (struct sockaddr_in *)&addr;
    port = ntohs(s->sin_port);
    inet_ntop(AF_INET, &s->sin_addr, ipstr, sizeof ipstr);
    printf("\n\nIdentify: received new request from %s port %d\n",ipstr,port);
    return;
}


// Reads a line (of at most 'n' bytes) from socket
int read_line(int socket,int n)
{
    int n_read;
    int not_eol;
    int ret;
    char new_char;
    n_read=0;
    not_eol=1;
    while (n_read<n && not_eol) {
        ret = read(socket,&new_char,sizeof(char));
        if (ret == -1) {
            printf("Error reading from socket (read_line)");
            return -1;
        }
        else if (ret == 0) {
            return 0;
        }
        else if (new_char=='\r') {
            not_eol = 0;
            // consumes next byte on buffer (LF)
            read(socket,&new_char,sizeof(char));
            continue;
        }
        else {
            buf[n_read]=new_char;
            n_read++;
        }
    }
    buf[n_read]='\0';
#if DEBUG
    printf("read_line: new line read from client socket: %s\n",buf);
#endif
    return n_read;
}


// Creates, prepares and returns new socket
int fireup(int port)
{
    int new_sock;
    struct sockaddr_in name;
    // Creates socket
    if ((new_sock = socket(PF_INET, SOCK_STREAM, 0))==-1) {
        printf("Error creating socket\n");
        return -1;
    }
    // Binds new socket to listening port
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    name.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(new_sock, (struct sockaddr *)&name, sizeof(name)) < 0) {
        printf("Error binding to socket\n");
        return -1;
    }
    // Starts listening on socket
    if (listen(new_sock, 5) < 0) {
        printf("Error listening to socket\n");
        return -1;
    }
    return(new_sock);
}


// Sends a 404 not found status message to client (page not found)
void not_found(int socket)
{
    sprintf(buf,"HTTP/1.0 404 NOT FOUND\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,SERVER_STRING);
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"Content-Type: text/html\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"<HTML><TITLE>Not Found</TITLE>\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"<BODY><P>Resource unavailable or nonexistent.\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"</BODY></HTML>\r\n");
    send(socket,buf, strlen(buf), 0);
    return;
}


// Send a 5000 internal server error (script not configured for execution)
void cannot_execute(int socket)
{
    sprintf(buf,"HTTP/1.0 500 Internal Server Error\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"Content-type: text/html\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"\r\n");
    send(socket,buf, strlen(buf), 0);
    sprintf(buf,"<P>Error prohibited CGI execution.\r\n");
    send(socket,buf, strlen(buf), 0);
    return;
}


// Closes socket before closing
void catch_ctrlc(int sig)
{
    
    int i;
    printf("Server terminating\n");
    // kill configuration manager and statistics manager process
    kill(childs[0], SIGKILL);
    kill(childs[1],SIGKILL);
    // close server socket
    close(socket_conn);
    // kill threads and close semaphores
    for(i=0;i<shared_mem->n_threads;i++)
    {
        sem_close(pool[i].sem);
        pthread_kill(pool[i].mythread,0);
    }
    pthread_kill(scheduler_thread,0);
    // close already existing client sockets
    for(i=0;i<shared_mem->n_threads*2;i++)
    {
        close(requests[i].client_socket);
    }
    shmdt(shared_mem);
    shmctl(shmid, IPC_RMID, NULL);
    // kill main process
    exit(0);
}

