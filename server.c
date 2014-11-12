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

// Produce debug information
#define DEBUG	  	1	

// Header of HTTP reply to client 
#define	SERVER_STRING 	"Server: simpleserver/0.1.0\r\n"
#define HEADER_1	"HTTP/1.0 200 OK\r\n"
#define HEADER_2	"Content-Type: text/html\r\n\r\n"

#define GET_EXPR	"GET /"
#define CGI_EXPR	"cgi-bin/"
#define SIZE_BUF	1024

//----------------------------------------------------------------------------------------------ADDED-----------------------------------------------------------------------

#define N_THREADS 5 												// por agora, depois tem de ser lido do ficheiro de configuração
typedef struct request
{
	int type;														// 1 dinamico, 0 estatico
	char file[SIZE_BUF];
	int client_socket;
}Request;

//-----------------------------------------------------------------------------------------------END---------------------------------------------------------------------

int  fireup(int port);
void identify(int socket);
void get_request(int socket);
int  read_line(int socket, int n);
void send_header(int socket);
void send_page(int socket);
void execute_script(int socket);
void not_found(int socket);
void catch_ctrlc(int);
void cannot_execute(int socket);
void *scheduler(void *id);
void acceptRequests();
void configuration_manager();
void statistics_manager();
void *dispatchRequests(void*id);


char buf[SIZE_BUF];
char req_buf[SIZE_BUF];
char buf_tmp[SIZE_BUF];
int port,socket_conn;


// ----------------------------------------------------------------------------ADDED VARIABLES------------------------------------------------------------------
pid_t childs[2];
pthread_t mythreads[N_THREADS+1];		// one for the scheduler thread
int id_threads[N_THREADS];
pthread_cond_t go_on = PTHREAD_COND_INITIALIZER;
pthread_mutex_t lock_thread = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t lock_head = PTHREAD_MUTEX_INITIALIZER;	
Request buffer[N_THREADS*2];
int read_pos = 0,write_pos =0,head,buffer_size=0;	
//-------------------------------------------------------------------------------END--------------------------------------------------------------------------



int main(int argc, char ** argv)
{
	int port,i;
	signal(SIGINT,catch_ctrlc);
	// Verify number of arguments
	if (argc!=2) {
		printf("Usage: %s <port>\n",argv[0]);
		exit(1);
	}
	port=atoi(argv[1]);
	printf("Listening for HTTP requests on port %d\n",port);

	// Configure listening port - Creates, prepares and returns new socket for server to listen clients requests
	if ((socket_conn=fireup(port))==-1)
		exit(1);
	//-----------------------------------------------------------------------------------CREATE CONFIGURATION MANAGER PROCESS,STATISTICS MANAGER PROCESS AND THREAD POOL------------------------------------------------------
	if( (childs[0] = fork()) ==0)
	{
		configuration_manager();
	}
	if((childs[1] = fork()) ==0)
	{
		statistics_manager();	
	}
	head = 0;
	for(i=0;i<N_THREADS+1;i++)
	{
		id_threads[i] = i;
		if(i== N_THREADS)
		{
			if(pthread_create(&mythreads[i],NULL,scheduler,&id_threads[i])!=0)
			{
				perror("\nError creating thread !\n");
			}
		}
		else
		{	if(pthread_create(&mythreads[i],NULL,dispatchRequests,&id_threads[i])!=0)
			{
				perror("\nError creating thread !\n");
			}
		}
	}
	//-----------------------------------------------------------------------------------------------------------END---------------------------------------------------------------------------------------------
	while(1)
	{
		acceptRequests();
	}
	for(i=0;i<N_THREADS+1;i++)
	{
		pthread_join(mythreads[i],NULL);
	}
	while(wait(NULL)!= -1)
	{
		;
	}
	return 0;
}




// ----------------------------------------------------------------------------------------------------------scheduler thread--------------------------------------------------------------------------------

void *scheduler(void *id)
{
	int my_id = *((int*) id);
	while(1)
	{
		printf("\nI am the scheduler, my id is %d !\n",my_id);
		sleep(50);
	}
}

//--------------------------------------------------------------------------------------------------------threads accepting requests-----------------------------------------------------------------------

void acceptRequests()
{
	int new_conn,aux_type;
	struct sockaddr_in client_name;
	socklen_t client_name_len = sizeof(client_name);
	// Accept connection on socket
	if ( (new_conn = accept(socket_conn,(struct sockaddr *)&client_name,&client_name_len)) == -1 ) {
		printf("Error accepting connection\n");
		exit(1);
	}
	// Identify new client
	identify(new_conn);
	// Process request
	get_request(new_conn);
	// Verify if request is for a page or script
	if(!strncmp(req_buf,CGI_EXPR,strlen(CGI_EXPR)))
		aux_type = 1;	
	else
		// Search file with html page and send to client
		aux_type = 0;

	// add request to requests buffer
	buffer[write_pos].type = aux_type;
	buffer[write_pos].client_socket = new_conn;
	strcpy(buffer[write_pos].file,"");
	strcpy(buffer[write_pos].file,req_buf);
	write_pos = (write_pos +1)%N_THREADS;
	buffer_size++;
	pthread_cond_broadcast(&go_on);
}

//---------------------------------------------------------------------------------------------------------dispatch requests-----------------------------------------------------------------------------------

void *dispatchRequests(void*id)
{
	int my_id = *((int*) id);
	while(1)
	{
		pthread_mutex_lock(&lock_thread);
		while(head != my_id || buffer_size==0)
		{
			pthread_cond_wait(&go_on,&lock_thread);
		}
		pthread_mutex_unlock(&lock_thread);
		//Verify if request is for a page or script
		if(buffer[read_pos].type ==1)
			execute_script(buffer[read_pos].client_socket);	
		else
			// Search file with html page and send to client
			send_page(buffer[read_pos].client_socket);
		pthread_mutex_lock(&lock_head);
		head = (head +1)%N_THREADS;
		read_pos = (read_pos +1)%N_THREADS;
		pthread_mutex_unlock(&lock_head);
		// Terminate connection with client
		buffer_size--;
		close(buffer[read_pos].client_socket);
		pthread_cond_broadcast(&go_on);
		return id;
	}
}

//---------------------------------------------------------------------------------------------------------configuration manager process------------------------------------------------------------------------

void configuration_manager()
{
	while(1)
	{
		printf("I am the configuration_manager process.\n");
		sleep(50);
	}
}


//----------------------------------------------------------------------------------------------------------statistics manager process---------------------------------------------------------------------------

void statistics_manager()
{
	while(1)
	{
		printf("I am the statistics manager process.\n");
		sleep(50);
	}
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
void execute_script(int socket)
{
	// Currently unsupported, return error code to client
	cannot_execute(socket);
	
	return;
}


// Send html page to client
void send_page(int socket)
{
	FILE * fp;

	// Searchs for page in directory htdocs
	sprintf(buf_tmp,"htdocs/%s",req_buf);

	#if DEBUG
	printf("send_page: searching for %s\n",buf_tmp);
	#endif

	// Verifies if file exists
	if((fp=fopen(buf_tmp,"rt"))==NULL) {
		// Page not found, send error to client
		printf("send_page: page %s not found, alerting client\n",buf_tmp);
		not_found(socket);
	}
	else {
		// Page found, send to client 
	
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
	kill(childs[0], SIGKILL);
	kill(childs[1],SIGKILL);
	close(socket_conn);
	pthread_mutex_destroy(&lock_thread);
	pthread_mutex_destroy(&lock_head);
	pthread_cond_destroy(&go_on);
	for(i=0;i<N_THREADS+1;i++)
	{
		pthread_kill(mythreads[i],0);
	}
	exit(0);
}

