// Produce debug information
#define DEBUG	 0
#define TEST 0
int zz=0;


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
#define REJECTED 5
#define FAVICON 999


typedef struct configuration{
    int server_port;
    int n_threads;
    int scheduling_policy;
    char scripts_list[5][10]; // maximum of 5 scripts authorized
}Configuration;


typedef struct thread_pool
{
	pthread_t mythread;
	sem_t *sem;
	int occupied;
    int position;
}Thread_pool;

typedef struct request
{
    int type;                                                       
    char file[SIZE_BUF];
    int client_socket;
    int dispatched;
    int occupied;
}Request;


typedef struct message
{
	long mtype;
	int type;
	char file[SIZE_BUF];
	int n_thread;
	char reception_hour[20];
	char final_hour[20];
}Message;


int  fireup(int port);
void identify(int socket);
void get_request(int socket);
int  read_line(int socket, int n);
void send_header(int socket);
int send_page(Request temp);
int execute_script(Request temp);
void not_found(int socket);
void catch_signals(int);
void cannot_execute(int socket);
void init();
void configuration_manager();
void statistics_manager();
void *scheduler();
void *dispatchRequests(void *id);
void buffer_full(int socket);
int is_valid(Request temp);
Message create_Message(int type,char file[SIZE_BUF],int n_thread,char reception_hour[20],char final_hour[20]);
void write_file(Message temp);
void catch_sighup(int sig);
void clean_up();
void load_configurations();
void catch_sighup_config(int sig);
void restart_server();
void not_authorized(int socket);


char buf[SIZE_BUF];
char req_buf[SIZE_BUF];
char buf_tmp[SIZE_BUF];
int port,socket_conn,new_conn;

int scheduling_policy;
int *id_threads,read_pos=0,write_pos = 0,mqid,static_accesses=0,dynamic_accesses=0,rejected_accesses=0,shmid;
pid_t main_process,childs[2];
pthread_t scheduler_thread;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t *stop_scheduler,*threadlist, *loadfileschecker;
Request *requests; // buffer
Thread_pool *pool;
time_t beginning;
char start_time[20];
Configuration *shared_mem;
sigset_t mask;  