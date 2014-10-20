/*
 * 	Team Members:
 *
 *  Member1:
 *	Name: Srivaths Ranganathan
 *	Andrew ID: srivathr
 *
 *  Member2:
 *  Name: Sandeep Vadlaputi
 *  Andrew ID: svadlapu
 * 
 * This proxy server takes a port number as input and listens to incoming
 * connections on that port. It spawns a new thread for each new client that 
 * tries to connect and severs to each of those clients. It takes the URL to 
 * access from client and checks if that resource is already present in the
 * cache. If not presnt in the cache, it connects to that host port if the URL
 * is valid and sends the resource as a response to the client after caching the 
 * request and the corresponding response. 
 * 
 * The cache works by evicting the approximately least recently used resource.
 * 
 * Our proxy server also handles dynamic content seperately, by not caching it 
 * at all to preserve the correctness of the proxy server's cache in cases where
 * the response may change on repeated calls.
 * [ Ex: host-server that responds with current data and time]
 *
 * The proxy server also handles the SIGPIPE signal efficiently and ensures that
 * it does not terminate on receiving this signal.
 *
 * csapp.c has also been modified and the Rio_* functions edited to ensure that
 * the proxy server doesn't terminate on ECONNRESET or EPIPE.
 */

#include <stdio.h>
#include <limits.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including these long lines in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *accept_hdr = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n";
static const char *accept_encoding_hdr = "Accept-Encoding: gzip, deflate\r\n";
static const char *connection_hdr = "Connection: close\r\n";
static const char *proxy_conn_hdr = "Proxy-Connection: close\r\n";

/* Structure for populating the content from the uri */
struct req_content {
	char host[MAXLINE];
	char path[MAXLINE];
	int port;
};

/* A cache object/block */
typedef struct cache_object {
	char tag[MAXLINE];
	char data[MAX_OBJECT_SIZE];
	int timestamp;
	pthread_rwlock_t rwlock;
	struct cache_object *next;
}cache_object;

int cache_size = 0, timer = 0;
cache_object *cache_start = NULL; //Head pointer for cache list

// lock that synchronises adding of new blocks to the cache:
pthread_rwlock_t cache_start_rwlock; 

/* Throw a HTML style error message to the client */
void clienterror(int fd, char *cause, char *errnum, 
		char *shortmsg, char *longmsg) {
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The AWESOME Proxy server</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

/* Extract the hostname, path to resource and port number from the URL
 *
 */
void parse_uri(char *uri, struct req_content *content) {
	char temp[MAXLINE];
	
	//Extract the path to the resource
	if(strstr(uri,"http://") != NULL)
	       sscanf( uri, "http://%[^/]%s", temp, content->path); 
	else
		sscanf( uri, "%[^/]%s", temp, content->path); 
		
	//Extract the port number and the hostname
	if( strstr(temp, ":") != NULL)
		sscanf(temp,"%[^:]:%d", content->host, &content->port);	
	else {
		strcpy(content->host,temp);
		content->port = 80;
	}
	
	// incase the path to resource is empty
	if(!content->path[0])
		strcpy(content->path,"./");

}

/*
 * Reads the request headers from the client and generates a new request header
 * header to send to the host that the client is trying to access.
 * Ensures that the headers follow the writup specification.
 * If the host header is present in the reqest headers frm the client it copies
 * it directly. Else it generates it using the hostname and adds it. 
 */
int read_requesthdrs(rio_t *rp, char *data) {
    char buf[MAXLINE];
    int ret = 0;
    
    do {
    	Rio_readlineb(rp, buf, MAXLINE);

    	if(!strcmp(buf, "\r\n"))
    		continue;

    	if(strstr(buf, "User-Agent:")) 
    		continue;

    	if(strstr(buf, "Accept:"))
    		continue;

    	if(strstr(buf, "Accept-Encoding:"))
    		continue;

    	if(strstr(buf, "Connection:"))
    		continue;

    	if(strstr(buf, "Proxy-Connection:")) 
    		continue;
    	
    	if(strstr(buf, "Host:")) {
    		sprintf(data, "%s%s", data, buf);
    		ret = 1;
    		continue;
    	}

    	sprintf(data, "%s%s", data, buf);
    	
    }while(strcmp(buf, "\r\n"));
    return ret;
}

/*
 * This method searches for the requested resource in the cache and returns 
 * the pointer to the cache block which contains the resource.
 * Returns NULL otherwise.
 */
cache_object *check_cache_hit(char *tag) {
	
	cache_object *ptr = cache_start;

	while(ptr != NULL) 
	{
		//If block is locked, skip it and search the next one.
		if(pthread_rwlock_trywrlock(&(ptr->rwlock)) == 0)
		{
			if(!strcmp(ptr->tag, tag)) {
				return ptr;
			}
			// unlock block if the tag doesn't match
			pthread_rwlock_unlock(&(ptr->rwlock));
		}
		ptr = ptr->next;
	}
	return NULL;
}

// Read response from the cache
void read_cache_data(cache_object *hit_addr, char *response) 
{
	
	strcpy(response, hit_addr->data);
	hit_addr->timestamp = timer;
	pthread_rwlock_unlock(&(hit_addr->rwlock));
	return;
}

/* 
	* Function to cache the response received from the server. The function 
	checks to see if the response is within allowed object size and is in limit 
	with the allowed cache size. 
	* If the cache is free, a new cache object is added. Else an existing block
	is evicted and the old content is replaced with the new data.
	* The writes and reads performed are thread-safe.
*/
void write_to_cache(char *tag, char *data, int size) {
	if(size > MAX_OBJECT_SIZE) return;

	//Cache is not full - Add a new block without eviction
	if(cache_size + size <= MAX_CACHE_SIZE) {
		//Creating node
		//Locking cache_start here
		pthread_rwlock_wrlock(&cache_start_rwlock);
		cache_object *object = Malloc(sizeof(cache_object));
		strcpy(object->data, data);
		strcpy(object->tag, tag);
		object->timestamp = timer;
		//Adding node to the front of the list
		object->next = cache_start;
		//Initialize rwlock for new block.
		pthread_rwlock_init(&(object->rwlock), NULL);
		cache_start = object;
		cache_size += size;
		//Unlocking cache_start here.
		pthread_rwlock_unlock(&cache_start_rwlock);
	}
	//Cache is full - Evict a LRU block and add new data
	else {
		//Evict/Modify node with new data
		int lru = INT_MAX;
		cache_object *victim, *ptr;
		ptr = cache_start;
		//Find victim from the cache list
		while(ptr != NULL) {
			//If block is being used, ignore and move to next
			if(pthread_rwlock_trywrlock(&(ptr->rwlock)) == 0)
			{
				if(ptr->timestamp < lru) {
					// unlock the old victim 
					pthread_rwlock_unlock(&(victim->rwlock));
					lru = ptr->timestamp;
					victim = ptr;
				}
				else
					pthread_rwlock_unlock(&(ptr->rwlock));	
			}
			ptr=ptr->next;
		}
		//Modify victim
		strcpy(victim->data, data);
		strcpy(victim->tag, tag);
		victim->timestamp = timer;
		pthread_rwlock_unlock(&(victim->rwlock));
	}
}

/* 	Core part of the proxy server that parses the request from the client, 
		forwards it to the server and sends the response (cached or real-time)  
		back to the client.
	The proxy is LRU cache enabled with synchronization.
*/
void doit(int fd) {
	char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
	char hdr_data[MAXLINE], new_request[MAXBUF], response[1<<15];
	rio_t rio1;
	struct req_content content;
	int host_mentioned, clientfd;
	int is_dynamic = 0; // Stores whether the content requested is dynamic
	
	timer++; //Refresh the clock with every request - for cache purposes

	Rio_readinitb(&rio1, fd);
	
	Rio_readlineb(&rio1, buf, MAXLINE);
	
	sscanf(buf, "%s %s %s", method, uri, version);
	
	//This proxy currently only supports the GET call.
	if(strcasecmp(method, "GET")) {
		clienterror(fd, method, "501", "Not implemented", 
			"This proxy does not implement this method");
		return;
	}
	
	/* if the resource requested is in the "cgi-bin" directory then the content
	 * is dynamic
	 */
	if(strstr(uri, "cgi-bin")) is_dynamic = 1;

	//Parse the HTTP request to extract the hostname, path and port.
	parse_uri(uri, &content);

	cache_object *hit_addr;

	//Check if the requested content is not dynamic and is already in cache.
	if( (!is_dynamic) && ((hit_addr = check_cache_hit(uri)) != NULL))//Cache hit
		read_cache_data(hit_addr, response);

	else { //Cache miss
	
		//Parse the request headers.
		host_mentioned = read_requesthdrs(&rio1, hdr_data);
		
		//Generate a new modified HTTP request to forward to the server
		sprintf(new_request, "GET %s HTTP/1.0\r\n", content.path);

		if(!host_mentioned)
			sprintf(new_request, "%sHost: %s\r\n", new_request, content.host);

		strcat(new_request, hdr_data);
		strcat(new_request, user_agent_hdr);
		strcat(new_request, accept_hdr);
		strcat(new_request, accept_encoding_hdr);
		strcat(new_request, connection_hdr);
		strcat(new_request, proxy_conn_hdr);
		strcat(new_request, "\r\n");
		
		//Create new connection with server
		clientfd = Open_clientfd_r(content.host, content.port);

		//Write HTTP request to the server
		Rio_writen(clientfd, new_request, sizeof(new_request));
		
		//Read response from the server
		int response_size = Rio_readn(clientfd, response, sizeof(response));
		
		//Cache a copy of the response for future requests
		if(!is_dynamic)
			write_to_cache(uri, response, response_size);
		
		Close(clientfd);
	}
	
	//Forward the response to the client
	Rio_writen(fd, response, sizeof(response));
}

/* Thread-handler that calls the required functions to serve the requests. */
void thread_wrapper(void *vargs) {
	int fd = *(int *) vargs;
	Pthread_detach(pthread_self());
	Free(vargs);
	doit(fd);
	Close(fd);
	Pthread_exit(NULL);
}

void sig_handler(int sig) {
	printf("SIGPIPE signal trapped. Exiting thread.");
	Pthread_exit(NULL);
}

/* Main function: Listens on a specific port for requests from clients
	and serves them by contacting servers.
*/
int main(int argc, char **argv)
{

	Signal(SIGPIPE, sig_handler); // Ignore SIGPIPE

    int listenfd, port;
    int *connfd;
	struct sockaddr_in clientaddr;
	pthread_t tid;
	cache_start = NULL; //Initializing the cache
	
	//Initialize lock for cache_start to maintain thread-safe operation
	pthread_rwlock_init(&(cache_start_rwlock), NULL);
	
	int clientlen = sizeof(clientaddr);	
	if(argc != 2) {
		fprintf(stderr, "usage: %s <port>\n", argv[0]);
		exit(1);
	}

	port = atoi(argv[1]);
	
	listenfd = Open_listenfd(port);
	
	//Start listening on port 'port' for incoming requests and serve them.
	while(1) {
		connfd = (int *) Malloc(sizeof(connfd));
		
		*connfd = Accept(listenfd, (SA *)&clientaddr, (socklen_t *)&clientlen);
		
		//Start a new thread for every new request.
		Pthread_create(&tid, NULL, (void *)thread_wrapper, connfd);
	}
	
	//Freeing up the cache
	cache_object *ptr = cache_start;
	while(ptr != NULL) {
		cache_object *temp = ptr->next;
		Free(ptr);
		ptr = temp;
	}

    return 0;
}