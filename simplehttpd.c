/* 
 * -- simplehttpd.c --
 * A (very) simple HTTP server
 * João Paulo dos Reis Gonçalves     2012142709
 * João Miguel Borges Subtil         2012151975 
 * Tempo dispendido no projecto : 90 horas
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
#include <time.h>
#include <sys/msg.h>
#include "simplehttpd.h"

int main(int argc, char ** argv)
{
    main_process = getpid();
	struct sockaddr_in client_name;
	socklen_t client_name_len = sizeof(client_name);
	int i,found_slot;
	Message toSend;
    // prevent unexpected interruptions
    sigemptyset(&mask);
    sigfillset(&mask);
    sigprocmask(SIG_BLOCK,&mask,NULL);
    init();
    sigprocmask(SIG_UNBLOCK,&mask,NULL);
    sigdelset(&mask,SIGINT);
    sigdelset(&mask,SIGUSR1);
    sigprocmask(SIG_BLOCK,&mask,NULL);
    signal(SIGINT,catch_signals);
    signal(SIGUSR1,restart_server);    
	// Serve requests 
	while (1)
	{
        
        #if DEBUG
        for(i=0;i<shared_mem->n_threads*2;i++)
        {
            printf("Pedido[%d], dispatched=%d  file=%s\n",i,requests[i].dispatched,requests[i].file);
        }
        printf("MY_ID IS =%d\n",getpid());
        for(i=0;i<(shared_mem->n_threads);i++)
        {
            printf("Threadsid[%d]=%d, occupied=%d\n",i,id_threads[i],pool[i].occupied);
        }
        #endif
        found_slot = FALSE;
		// Accept connection on socket
		if ( (new_conn = accept(socket_conn,(struct sockaddr *)&client_name,&client_name_len)) == -1 ) 
		{
			printf("Error accepting connection\n");
			//clean_up();
		}
		// search for a space in requests buffer and update write_pos 
		#if DEBUG
		printf("Searching for an available slot in the requests buffer.\n");
		#endif
		// Identify new client
        identify(new_conn);
        // Process request
        get_request(new_conn);
        #if TEST
        printf("FAVICON result=%d and filename is %s\n",strcmp(req_buf,"favicon.ico"),req_buf);
        // if not index.html
        #endif
        
		for(i=0;i<shared_mem->n_threads*2;i++)
        {
            if(strcmp(req_buf,"favicon.ico")==0)    // avoid favicon requests
            {
                found_slot=FAVICON;
                break;
            }
            else if(requests[i].dispatched == TRUE && requests[i].occupied == FALSE) // if buffer has space
            {
                found_slot = TRUE;
                write_pos = i;
                break;
            }
        }
         
        if(found_slot == FALSE)         // if buffer has no space, request is rejected
        {
          #if TEST
            printf(">>>>>>>>>>>>>>>>>>Request rejected <<<<<<<<<<<<<<<\n");
          #endif
           	toSend = create_Message(REJECTED," ",0," "," ");
			msgsnd(mqid,&toSend,sizeof(toSend)-sizeof(long),0);
            buffer_full(new_conn);
            continue;
        }
        else if (found_slot == FAVICON)
        {
            #if TEST
            printf("FAVICON ALERT\n");
            #endif
            continue;
        }
       
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
        #if TEST
                printf(">>>>>>>>>>>>>>>>>>>>>Request added to position :  %d file requested : %s\n ",write_pos,requests[write_pos].file);
        #endif
		// awake the scheaduler thread to search which is the next available thread and the next request to be dispatched
      
    #if TEST
		printf("Alerting the scheduler thread that threre is new requests.\n");
	#endif
		sem_post(stop_scheduler);
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
    //create shared memory
    shmid = shmget(IPC_PRIVATE,sizeof(Configuration),IPC_CREAT|0777);
    shared_mem = shmat(shmid,NULL,0);
    // semaphore that guarantees that configurations are totally loaded
    sprintf(aux,"LOADFILECHECKER");
    sem_unlink(aux);
    loadfileschecker=sem_open(aux,O_CREAT|O_EXCL,0700,0); 
    if( (childs[0] = fork()) ==0)
    {
        configuration_manager();
        exit(0);
    }
    sem_wait(loadfileschecker);
    //dynamic memory
    id_threads=malloc((shared_mem->n_threads)*sizeof(id_threads));
    requests=malloc((shared_mem->n_threads*2)*sizeof(Request));
    pool=malloc((shared_mem->n_threads*2)*sizeof(Thread_pool));
    port=shared_mem->server_port;
    scheduling_policy=shared_mem->scheduling_policy;//2-static 3-dynamic ou 4-fifo
    // Configure listening port
    if ((socket_conn=fireup(shared_mem->server_port))==-1)
        exit(1);
	// hour that the server started
	beginning = time(NULL);
    strftime(start_time, 20, "%Y-%m-%d %H:%M:%S", localtime(&beginning));
    // open logs.txt file
	FILE *fp = fopen("logs.txt","w");
	if(fp == NULL)
	{
		printf("Error opening logs file.\n");
		exit(0);
	}
	fclose(fp);
	/* create message queue */
 	mqid = msgget(IPC_PRIVATE, IPC_CREAT|0777);
  	if(mqid < 0)
    {
      perror("Error creating message queue.\n");
      exit(0);
    }
	// open scheduler semaphore
	sem_unlink("STOP_SCHEDULER");
   	stop_scheduler = sem_open("STOP_SCHEDULER",O_CREAT|O_EXCL,0700,0);
    // set the initial requests to dispatched, to say to the dispatch function that there is no requests yet
    for(i=0;i<(shared_mem->n_threads*2);i++)
    {
        requests[i].dispatched = TRUE;
        requests[i].occupied = FALSE;
        sprintf(requests[i].file, " ");
        
    }
	// initialize semaphores from thread pool
	for(i=0;i<(shared_mem->n_threads);i++)
	{
		id_threads[i] = i;
		pool[i].occupied = FALSE;
        pool[i].position=0;
		sprintf(aux,"SEM_%d",i);
		sem_unlink(aux);
    	pool[i].sem = sem_open(aux,O_CREAT|O_EXCL,0700,0);
	}
	//make semaphore to count active threads
    sprintf(aux,"THREADLIST");
    sem_unlink(aux);
    threadlist=sem_open(aux,O_CREAT|O_EXCL,0700,shared_mem->n_threads);
	//create  statistics manager process 
	if((childs[1] = fork()) ==0)
	{
		statistics_manager();
		exit(0);	
	}
	// create scheduler thread
	if(pthread_create(&scheduler_thread,NULL,scheduler,NULL)!=0)
	{
		perror("\nError creating scheduler thread !.\n");
		clean_up();
	}
	// create thread pool
	for(i=0;i<shared_mem->n_threads;i++)
	{
		if(pthread_create(&(pool[i].mythread),NULL,dispatchRequests,&id_threads[i])!=0)
		{
			perror("\nError creating dispatch thread !\n");
			clean_up();
		}
	}    
}



// configuration manager process
void configuration_manager()
{
    printf("I am the configuration_manager and my pid is %d\n",getpid() );
    load_configurations();
    sem_post(loadfileschecker); // alerting main process that configurations were loaded
    // prevent unexpected interruptions
    sigprocmask(SIG_UNBLOCK,&mask,NULL);
    sigdelset(&mask,SIGHUP);
    sigprocmask(SIG_BLOCK,&mask,NULL);
    signal(SIGHUP,catch_sighup_config);  
    while(1)                            // wait for a SIGHUP signal
    {   
        pause();
    }
}


// statistics manager process
void statistics_manager()
{
	printf("I am the statistics_manager and my pid is %d\n",getpid() );
    // prevent unexpected interruptions
    sigprocmask(SIG_UNBLOCK,&mask,NULL);
    sigdelset(&mask,SIGHUP);
    sigprocmask(SIG_BLOCK,&mask,NULL);
    signal(SIGHUP,catch_sighup);
    Message toReceive;
	while(1)                   // always waiting for new messages
	{
		msgrcv(mqid,&toReceive,sizeof(toReceive) -sizeof(long),1,0);
		if(toReceive.type == STATIC)
		{
			static_accesses++;
			write_file(toReceive);
		}
		else if(toReceive.type == DYNAMIC)
		{
			dynamic_accesses++;
			write_file(toReceive);
		}
		else if(toReceive.type == REJECTED)
		{
			rejected_accesses++;
		}
        toReceive.type = 0;
	}
}


// scheduler thread - decides which is the next thread and which is the next request to be dispatched 
void *scheduler()
{
    int i,found_request,j;
    while(1)
    {
        sem_wait(stop_scheduler);
        found_request = FALSE;
        // search the next request to be dispatched(FIFO, priority to static requests, priority to dynamic requests), it has to be valid
        sleep(2);
        // FIFO policy
    #if DEBUG
        printf("Searching the next request to be dispatched.\n");
    #endif
        if(scheduling_policy == FIFO)
        {
            for(i=read_pos,j=0;j<(shared_mem->n_threads*2);j++)
            {
               if(requests[(i+j) % (shared_mem->n_threads*2)].dispatched == FALSE && is_valid(requests[(i+j) % (shared_mem->n_threads*2)])== TRUE && requests[(i+j) % (shared_mem->n_threads*2)].occupied==FALSE)
               {
                   //se não foi despachado  é valido e ainda não foi processado
                    found_request = TRUE;
                    read_pos=(i+j) % (shared_mem->n_threads*2);
                    break;
                    
                }
            }
        }
        else if(scheduling_policy == STATIC)        // priority to static requests 2
        {
            for(i=read_pos,j=0;j<(shared_mem->n_threads*2);j++)
            {
                if(requests[(i+j) % (shared_mem->n_threads*2)].dispatched== FALSE && requests[(i+j) % (shared_mem->n_threads*2)].type == STATIC && requests[(i+j) % (shared_mem->n_threads*2)].occupied == FALSE)
                {
                    if(is_valid(requests[(i+j) % (shared_mem->n_threads*2)])== TRUE)
                    {
                        found_request = TRUE;
                        read_pos = (i+j) % (shared_mem->n_threads*2);
                        break;
                    }
                    else
                    {
                        requests[(i+j) % (shared_mem->n_threads*2)].dispatched = TRUE;
                    }
                }
            }
            if(found_request == FALSE)  // if there is no static requests, search for one of the other type
            {
                for (i=0;i<(shared_mem->n_threads*2);i++)
                {
                    if(requests[i].dispatched == FALSE && is_valid(requests[i])== TRUE && requests[i].occupied==FALSE)
                    {
                        //se não foi despachado  é valido e ainda não foi processado
                        found_request = TRUE;
                        read_pos=i;
                        break;
                        
                    }
                }
            }
        }
        else if(scheduling_policy == DYNAMIC)                                       // priority to dynamic requests   3
        {
            for(i=read_pos,j=0;j<(shared_mem->n_threads*2);j++)
            {
#if TESTE
                printf("requests[%d].dispatched=%d\n",i,requests[(i+j) % (shared_mem->n_threads*2)].dispatched);
#endif
                if(requests[(i+j) % (shared_mem->n_threads*2)].dispatched == FALSE && requests[(i+j) % (shared_mem->n_threads*2)].type == DYNAMIC && requests[(i+j) % (shared_mem->n_threads*2)].occupied==FALSE)
                {
                    if(is_valid(requests[(i+j) % (shared_mem->n_threads*2)]) == TRUE)
                    {
                        found_request = TRUE;
                        read_pos = (i+j) % (shared_mem->n_threads*2);
                        break;
                    }
                    else
                    {
                        requests[(i+j) % (shared_mem->n_threads*2)].dispatched = TRUE;
                    }
                }
                
            }
            if(found_request == FALSE)  // if there is no static requests, search for one of the other type
            {
                for (i=0;i<(shared_mem->n_threads*2);i++)
                {
                    if(requests[i].dispatched == FALSE && is_valid(requests[i])== TRUE && requests[i].occupied==FALSE)
                    {
                        //se não foi despachado  é valido e ainda não foi processado
                        found_request = TRUE;
                        read_pos=i;
                        break;
                        
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
        sem_wait(threadlist);
        for(i=0;i<shared_mem->n_threads;i++)
        {
            if(pool[i].occupied  == FALSE)
            {
                #if TEST
                printf("\n>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Signal thread number=%d to position =%d\n",i,read_pos);
                #endif
                pool[i].position=read_pos;
                sem_post(pool[i].sem);
                break;
            }
        }
    }
}





// dispatch requests
void *dispatchRequests(void *id)
{
	int my_id = *((int*) id);
	int old_read_pos;
	char r_buff[20];
	char f_buff[20];
	time_t reception_hour;
	time_t final_hour;
	Message toSend;
	int temp= FALSE;
    //sem_wait(pool[my_id].sem);
	while(1)
	{
		// thread waits for its time to execute
		sem_wait(pool[my_id].sem);
    	// get reception time
    	reception_hour = time(NULL);
    	strftime(r_buff, 20, "%Y-%m-%d %H:%M:%S", localtime(&reception_hour));
        // mutual exclusion zone  
        pthread_mutex_lock(&mutex);
        old_read_pos = pool[my_id].position;
        pool[my_id].occupied = TRUE;
        requests[old_read_pos].occupied=TRUE;
        pthread_mutex_unlock(&mutex);
        #if TEST
        printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Thread %d will dispatch the request %s.\n",my_id,requests[old_read_pos].file);
        #endif
        //Verify if request is for a page or script
		if(requests[old_read_pos].type == DYNAMIC)
			temp = execute_script(requests[old_read_pos]);	
		else
			// Search file with html page and send to client
			temp = send_page(requests[old_read_pos]);
		if(temp == TRUE)
		{
			//get final time
			final_hour = time(NULL);
    		strftime(f_buff, 20, "%Y-%m-%d %H:%M:%S", localtime(&final_hour));
			// get Message
			toSend = create_Message(requests[old_read_pos].type,requests[old_read_pos].file,my_id,r_buff,f_buff);
			// send Message
			msgsnd(mqid,&toSend,sizeof(toSend)-sizeof(long),0);
		}
		// Terminate connection with client
		close(requests[old_read_pos].client_socket);
    #if TEST
        printf("Thread %d vai dormir.\n",my_id);
        sleep(20);
        printf("Thread %d acordada.\n",my_id);
    #endif 
        //this request is already dispatched
        //pthread_mutex_lock(&mutex);
        requests[old_read_pos].occupied=FALSE;
        requests[old_read_pos].dispatched = TRUE;//está aqui para encher o buffer
        pool[my_id].occupied = FALSE;// change the status of the thread to waiting for execute
        // alerting semaphore that count active threads
    #if TEST
        int i;
        for(i=0;i<shared_mem->n_threads*2;i++)
        {
            printf("Pedido[%d], dispatched=%d  file=%s\n",i,requests[i].dispatched,requests[i].file);
        }
    #endif 
      //  pthread_mutex_unlock(&mutex);
        sem_post(threadlist);
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
	char aux[SIZE_BUF],*s;
    int authorized = FALSE,i;
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
        s = strtok(aux,"/");
        s = strtok (NULL, "/");
        s = strtok(NULL,"/");
        printf("COMPARE %s\n",s );
		for(i=0;i<5;i++)
        {
            if(strcmp(s,shared_mem->scripts_list[i])==0)
            {
                authorized = TRUE;
            }
        }
        if(authorized == FALSE)
        {
            printf("is_valid: script %s not authorized alerting client\n",aux);
            not_authorized(temp.client_socket);
            close(temp.client_socket);
            return FALSE;
        }
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
		exit(1);
	}
	// If no particular page is requested then we consider htdocs/index.html
	if(!strlen(req_buf))
		sprintf(req_buf,"index.html");
	#if DEBUG
	printf("get_request: client requested the following page: %s\n",req_buf);
	#endif
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
int execute_script(Request temp)
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
		return FALSE;
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
	return TRUE;
}


// Send html page to client
int send_page(Request temp)
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
	return TRUE; 
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
	printf("identify: received new request from %s port %d\n",ipstr,port);
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
	int new_sock,so_reuseaddr = TRUE;
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
    setsockopt(new_sock,SOL_SOCKET,SO_REUSEADDR,&so_reuseaddr,sizeof so_reuseaddr);
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


// script not authorized to execute
void not_authorized(int socket)
{
    char *erro ="authorized.html";
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


Message create_Message(int type,char file[SIZE_BUF],int n_thread,char reception_hour[20],char final_hour[20])
{
	Message aux;
	aux.mtype = 1;
	aux.type = type;
	strcpy(aux.file,file);
	aux.n_thread = n_thread;
	strcpy(aux.reception_hour,reception_hour);
	strcpy(aux.final_hour,final_hour);
	return aux;
}



// write to logs file
void write_file(Message temp)
{
	char type[20];
	if(temp.type == STATIC)
	{
		sprintf(type,"static");
	}
	else
	{
		sprintf(type,"dynamic");

	}
	FILE *fp = fopen("logs.txt","a");
	if(fp == NULL)
	{
		printf("Error opening logs file at write_file.\n");
		return;
	}
	fprintf(fp,"%s,%s,%d,%s,%s\n",type,temp.file,temp.n_thread,temp.reception_hour,temp.final_hour);
	fclose(fp);
}



// load configurations to shared memory

void load_configurations()
{
	char buffer[100];
    char *str;
    FILE *fp=fopen("serverconfigs.txt","r+");
    char s[20];
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
        if(shared_mem->scheduling_policy == FIFO)
        {
            strcpy(s,"FIFO");
        }
        else if(shared_mem->scheduling_policy == STATIC)
        {
            strcpy(s,"STATIC");
        }
        else
        {
            strcpy(s,"DYNAMIC");
        }
        printf("ACTUAL CONFIGS\nport=%d\nnumber-threads=%d\npolicy=%s\n\n",shared_mem->server_port, shared_mem->n_threads,s);
    }

}




// catch sighup for configuration manager process
void catch_sighup_config(int sig)
{
    char option[2],aux_scripts_list[5][10];
    char aux_port[100],aux_policy[100];
    int aux_scripts,aux_threads,i=0; 
    printf("\nSIGHUP detected. Do you want to change the configurations ? ");
    scanf("%1s", option); 
    if (option[0] == 'y') 
    {
        printf("\nServer port : ");
        scanf("%s",aux_port);
        printf("\nNumber of threads : ");
        scanf("%d",&aux_threads);
        printf("\nScheduling policy(2.priority to static requests | 3.Priority to dynamic request. | 4.FIFO) : ");
        scanf("%s",aux_policy);
        printf("\nNumber of authorized scripts(0 to 5) : ");
        scanf("%d",&aux_scripts);
        for(i=0;i<aux_scripts;i++)
        {
            printf("\nAuthorized scripts : ");
            scanf("%s",aux_scripts_list[i]);
        }
        FILE *fp=fopen("serverconfigs.txt","w");
        if(fp == NULL)
        {
            printf("Error opening serverconfigs.\n");
            return;
        }
        else
        {
            fprintf(fp, "%s;%d;%s\n",aux_port,aux_threads,aux_policy);
            for(i=0;i<aux_scripts;i++)
            {
                fprintf(fp, "%s\n",aux_scripts_list[i]);
            }
            fclose(fp);
            printf("\nConfigurations changed successfully.\n");
        }
    }
    kill(main_process,SIGUSR1);     // send signal to restart
}

//catch sighup
void catch_sighup(int sig)
{	
	FILE *fp = fopen("logs.txt","r");
	if(fp == NULL)
	{
		printf("Error opening logs file at catch_sighup.\n");
		return;
	}
	char now[20],aux[SIZE_BUF];
	time_t actual_time = time(NULL);
    strftime(now, 20, "%Y-%m-%d %H:%M:%S", localtime(&actual_time));
	printf("\nServer started at : %s\n",start_time);
	printf("Total number of accesses to static content : %d\n",static_accesses);
	printf("Total number of accesses to dynamic content : %d\n",dynamic_accesses);
	printf("Total number of rejected accesses: %d\n",rejected_accesses);
	printf("Actual time : %s\n",now);
	printf("\n\nLogs : \n\n");
	while(fgets(aux,SIZE_BUF,fp))  // reading logs file
	{
		printf("%s\n",aux);
	}
}	


// Closes socket before closing
void catch_signals(int sig)
{	
	printf("Server terminating");
	clean_up();
}



void restart_server()
{
    printf("\nRestarting server...\n");
    int i;
    int error = 0,z;
    socklen_t len = sizeof (error);
    // closing server socket_connection
    struct linger so_linger;
    so_linger.l_onoff = TRUE;
    so_linger.l_linger = 0;
    z=setsockopt(socket_conn,SOL_SOCKET,SO_LINGER,&so_linger,sizeof(so_linger));
    if(z)
    {
        perror("error closing socket connection");
    }
    close(socket_conn);
    //remove message queue
    msgctl(mqid, IPC_RMID, 0);
    // kill configuration manager and statistics manager process
    kill(childs[0], SIGKILL);
    kill(childs[1],SIGKILL);
    // closing semaphores
    if(sem_close(stop_scheduler)==-1)
    {
            perror("Error closing stop_scheduler semaphore.\n");
    }
    if(sem_close(threadlist)==-1)
    {
            perror("Error closing threadlist semaphore.\n");
    }
    if(sem_close(loadfileschecker)==-1)
    {
            perror("Error closing loadfileschecker semaphore.\n");
    }
    // kill threads and close semaphores
    for(i=0;i<(shared_mem->n_threads);i++)  
    {
        if(sem_close(pool[i].sem)==-1)
        {
            perror("Error closing a thread_pool semaphore.\n");
        }
        if(pthread_kill(pool[i].mythread,0)!=0)
        {
            perror("Error killing a thread from thread_pool.\n");
        }
    }
    if(pthread_kill(scheduler_thread,0)!=0)
    {
        perror("Error killing scheduler_thread.\n");
    }
    // close already existing client sockets
    for(i=0;i<(shared_mem->n_threads*2);i++)
    {
        if(getsockopt (requests[i].client_socket, SOL_SOCKET, SO_ERROR, &error, &len )==0)
        {
            close(requests[i].client_socket);
        }
    }
    // detach and remove shared memory
    free(pool);
    free(id_threads);
    free(requests);
    shmdt(shared_mem);
    shmctl(shmid, IPC_RMID, NULL);
    //
    init();
}

void clean_up()
{
	int i;
    int error = 0,z;
    socklen_t len = sizeof (error);
    // closing server socket_connection
    struct linger so_linger;
    so_linger.l_onoff = TRUE;
    so_linger.l_linger = 0;
    z=setsockopt(socket_conn,SOL_SOCKET,SO_LINGER,&so_linger,sizeof(so_linger));
    if(z)
    {
        perror("error closing socket connection");
    }
    close(socket_conn);
	//remove message queue
	msgctl(mqid, IPC_RMID, 0);
	// kill configuration manager and statistics manager process
	kill(childs[0], SIGKILL);
	kill(childs[1],SIGKILL);
    // closing semaphores
    if(sem_close(stop_scheduler)==-1)
    {
            perror("Error closing stop_scheduler semaphore.\n");
    }
    if(sem_close(threadlist)==-1)
    {
            perror("Error closing threadlist semaphore.\n");
    }
    if(sem_close(loadfileschecker)==-1)
    {
            perror("Error closing loadfileschecker semaphore.\n");
    }
	// kill threads and close semaphores
	for(i=0;i<(shared_mem->n_threads);i++)	
	{
		if(sem_close(pool[i].sem)==-1)
		{
			perror("Error closing a thread_pool semaphore.\n");
		}
		if(pthread_kill(pool[i].mythread,0)!=0)
		{
			perror("Error killing a thread from thread_pool.\n");
		}
	}
	if(pthread_kill(scheduler_thread,0)!=0)
	{
		perror("Error killing scheduler_thread.\n");
	}
	// close already existing client sockets
	for(i=0;i<(shared_mem->n_threads*2);i++)
	{
        if(getsockopt (requests[i].client_socket, SOL_SOCKET, SO_ERROR, &error, &len )==0)
        {
            close(requests[i].client_socket);
        }
	}
	// detach and remove shared memory
    free(pool);
    free(id_threads);
    free(requests);
	shmdt(shared_mem);
    shmctl(shmid, IPC_RMID, NULL);
	// kill main process
	exit(0);
}
