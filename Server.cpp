#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/md5.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "support.h"
#include "Server.h"

int count_get = 0;

void help(char *progname)
{
	printf("Usage: %s [OPTIONS]\n", progname);
	printf("Initiate a network file server\n");
	printf("  -m    enable multithreading mode\n");
	printf("  -l    number of entries in the LRU cache\n");
	printf("  -p    port on which to listen for connections\n");
}

void die(const char *msg1, char *msg2)
{
	fprintf(stderr, "%s, %s\n", msg1, msg2);
	exit(0);
}

/*
 * open_server_socket() - Open a listening socket and return its file
 *                        descriptor, or terminate the program
 */
int open_server_socket(int port)
{
	int                listenfd;    /* the server's listening file descriptor */
	struct sockaddr_in addrs;       /* describes which clients we'll accept */
	int                optval = 1;  /* for configuring the socket */

	/* Create a socket descriptor */
	if((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		die("Error creating socket: ", strerror(errno));
	}

	/* Eliminates "Address already in use" error from bind. */
	if(setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int)) < 0)
	{
		die("Error configuring socket: ", strerror(errno));
	}

	/* Listenfd will be an endpoint for all requests to the port from any IP
	   address */
	bzero((char *) &addrs, sizeof(addrs));
	addrs.sin_family = AF_INET;
	addrs.sin_addr.s_addr = htonl(INADDR_ANY);
	addrs.sin_port = htons((unsigned short)port);
	if(bind(listenfd, (struct sockaddr *)&addrs, sizeof(addrs)) < 0)
	{
		die("Error in bind(): ", strerror(errno));
	}

	/* Make it a listening socket ready to accept connection requests */
	if(listen(listenfd, 1024) < 0)  // backlog of 1024
	{
		die("Error in listen(): ", strerror(errno));
	}

	return listenfd;
}

/*
 * handle_requests() - given a listening file descriptor, continually wait
 *                     for a request to come in, and when it arrives, pass it
 *                     to service_function.  Note that this is not a
 *                     multi-threaded server.
 */
void handle_requests(int listenfd, void (*service_function)(int, int), int param, bool multithread)
{
	while(1)
	{
		/* block until we get a connection */
		struct sockaddr_in clientaddr;
		memset(&clientaddr, 0, sizeof(sockaddr_in));
		socklen_t clientlen = sizeof(clientaddr);
		int connfd;
		if((connfd = accept(listenfd, (struct sockaddr *)&clientaddr, &clientlen)) < 0)
		{
			die("Error in accept(): ", strerror(errno));
		}

		/* print some info about the connection */
		struct hostent *hp;
		hp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
		if(hp == NULL)
		{
			fprintf(stderr, "DNS error in gethostbyaddr() %d\n", h_errno);
			exit(0);
		}
		char *haddrp = inet_ntoa(clientaddr.sin_addr);
		printf("server connected to %s (%s)\n", hp->h_name, haddrp);

		/* serve requests */
		service_function(connfd, param);

		/* clean up, await new connection */
		if(close(connfd) < 0)
		{
			die("Error in close(): ", strerror(errno));
		}
	}
}

char* hash_MD5(char* file_contents){
	unsigned char digest[MD5_DIGEST_LENGTH];
	MD5_CTX mdContext;
	MD5_Init(&mdContext);
	MD5_Update(&mdContext, file_contents, strlen(file_contents));
	MD5_Final (digest ,&mdContext);

	char* hashed_string = (char *)malloc(2*MD5_DIGEST_LENGTH*sizeof(char));
	bzero(hashed_string, MD5_DIGEST_LENGTH);
	for(int i = 0; i < MD5_DIGEST_LENGTH; i++){
		sprintf(&hashed_string[i*2], "%02x", digest[i]);
	}
	return hashed_string;
}

bool write_OK(int connfd, char* file_name){
	int OK_response_size = 3 + strlen(file_name) + 1;
	char OK_response[OK_response_size];
	sprintf(OK_response, "OK %s\n", file_name);
	if(write(connfd, OK_response, OK_response_size) < 0){
		fprintf(stderr, "%s", "Error writing OK\n");
		return false;
	}
	return true;
}

bool write_size(int connfd, long int file_size){
	if(write(connfd, &file_size, sizeof(file_size)) < 0){
		fprintf(stderr, "%s", "Error writing file size\n");
		return false;
	}
	return true;
}

bool write_hash(int connfd, char* hashed_file){
	if(write(connfd, hashed_file, 32) < 0){
		fprintf(stderr, "%s", "Error writing file hash\n");
		return false;
	}
	return true;
}

bool write_file(int connfd, char* file, long file_size){
	if(write(connfd, file, file_size) < 0){
		fprintf(stderr, "%s", "Error writing file contents\n");
		return false;
	}
	return true;
}


bool cached(int connfd, char* file_name, char** LRU, char** LRU_file_names, long int* LRU_file_sizes, char** LRU_hashes, int lru_size, bool checksum){

	for(int i = 0; i < lru_size; i++){
		if(LRU_file_names[i]){
			if(!strncmp(LRU_file_names[i], file_name, strlen(file_name))){
				if(write_OK(connfd, file_name)){
					if(write_size(connfd, LRU_file_sizes[i])){
						if(checksum){
							if(write_hash(connfd, LRU_hashes[i])){
								if(write_file(connfd, LRU[i], LRU_file_sizes[i])){
									return true;
								}
							}
						}
						else{
							if(write_file(connfd, LRU[i], LRU_file_sizes[i])){
								return true;
							}
						}
					}
				}
			}
		}
	}
	return false;
}

// static void update_LRU(char** LRU_file_names, long* LRU_file_sizes, char** LRU_hashes, char** LRU,
// 	char* file_name, long file_size, char* hashed_file, char* file_buffer, int lru_index, int lru_size){
// 	LRU_file_names[lru_index] = file_name;
// 	LRU_file_sizes[lru_index] = file_size;
// 	LRU_hashes[lru_index] = hashed_file;
// 	LRU[lru_index++] = file_buffer;
// 	if(lru_index == lru_size){
// 		lru_index = 0;
// 	}
// }

long int read_file_size(FILE* file){
	fseek(file, 0, SEEK_END);
	long int file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	return file_size;
}

/*
 * file_server() - Read a request from a socket, satisfy the request, and
 *                 then close the connection.
 */
void file_server(int connfd, int lru_size)
{

		const int MAXLINE = 8192;

		/* LRU Cache */
		/*
			Acts as a circular buffer
			lru_index: represents the LRU file that will respectively be evicted if the file requested is not in the cache
		*/

		static char **LRU = (char**)malloc(sizeof(char *)*lru_size);
		static char **LRU_file_names = (char**)malloc(sizeof(char *)*lru_size);
		static long int* LRU_file_sizes = (long int*)malloc(sizeof(long int)*lru_size);
		static char** LRU_hashes = (char**)malloc(sizeof(char *)*lru_size);
	 	static int lru_index = 0;
		static bool lru_initialized = false;

		if(!lru_initialized){
			for(int i = 0; i < lru_size; i++){
				LRU[i] = (char*)malloc(MAXLINE * sizeof(char));
				LRU_file_names[i] = (char*)malloc(MAXLINE * sizeof(char));
				LRU_file_sizes[i] = 0;
				LRU_hashes[i] = (char*)malloc(MAXLINE * sizeof(char));
			}
			lru_initialized = true;
		}

		char      buf[MAXLINE];
		bzero(buf, MAXLINE);
		read(connfd, buf, sizeof(buf));
		//printf("%s\n", buf);

		if(!strncmp(buf, "GET ", 4)){
			char* moving_buffer = buf;
			moving_buffer+=4;
			char* file_name = strtok(moving_buffer, "\n");
			if(!cached(connfd, file_name, LRU, LRU_file_names, LRU_file_sizes, LRU_hashes, lru_size, false)){
					FILE* get_file = fopen(file_name, "rb");
					if(get_file){
						write_OK(connfd, file_name);
						long int file_size = read_file_size(get_file);
						char *file_buffer = (char*)malloc(sizeof(char)*file_size);
						fread(file_buffer, file_size, 1, get_file);
						char* hashed_file = hash_MD5(file_buffer);
						write_size(connfd, file_size);
						write_file(connfd, file_buffer, file_size);
						if(lru_size > 0){
							sprintf(LRU_file_names[lru_index], "%s", (file_name));
							LRU_file_sizes[lru_index] = file_size;
							LRU_hashes[lru_index] = hashed_file;
							LRU[lru_index] = file_buffer;
							lru_index++;
							if(lru_index == lru_size){
								*(&lru_index) = 0;
							}
						}
					}
					else{
						fprintf(stderr, "GET - File not found %s\n", file_name);
					}
					fclose(get_file);
				}
		}
		else if(!strncmp(buf, "PUT ", 4)){
			char* moving_buffer = buf;
			moving_buffer+=4;
			const char* file_name = strtok(moving_buffer, "\n");
			FILE* put_file = fopen(file_name, "wb");
			if(put_file){
				moving_buffer += strlen(file_name) + 1;
				char* file_size_string = strtok(moving_buffer, "\n");
				long int file_size = atoi(file_size_string);
				moving_buffer += strlen(file_size_string) + 1;
				char file_contents[file_size];
				strncpy(file_contents, moving_buffer, file_size);
				moving_buffer+=file_size;
				fwrite(file_contents, file_size, 1, put_file);
			}
			else{
				perror("Error opening file for writing");
			}
			fclose(put_file);
		}
		else if(!strncmp(buf, "PUTC ", 5)){
			char* moving_buffer = buf;
			moving_buffer+=5;
			const char* file_name = strtok(moving_buffer, "\n");
			moving_buffer += strlen(file_name) + 1;
			char* MD5_digest = strtok(moving_buffer, "\n");
			moving_buffer += 33;

			char* file_size_string = strtok(moving_buffer, "\n");

			long int file_size = atoi(file_size_string);
			moving_buffer += strlen(file_size_string) + 1;
			char file_contents[file_size];
			strncpy(file_contents, moving_buffer, file_size);
			moving_buffer+=file_size;
			char* hashed_contents = hash_MD5(file_contents);
			printf("file_name: %s\n", file_name);
			printf("file_size: %d\n", file_size);
			printf("file_contents: %s\n", file_contents);
			printf("MD5_digest: %s\n", MD5_digest);
			printf("hashed_contents: %s\n", hashed_contents);
			if(!strncmp(hashed_contents, MD5_digest, 32)){
				FILE* put_file = fopen(file_name, "wb");
				if(put_file){
					fwrite(file_contents, file_size, 1, put_file);
				}
				fclose(put_file);
			}
			else{
				perror("MD5 does not match");
			}
			free(hashed_contents);
		}
		else if (!strncmp(buf, "GETC ", 5)){
			char* moving_buffer = buf;
			moving_buffer+=5;
			char* file_name = strtok(moving_buffer, "\n");
			if(!cached(connfd, file_name, LRU, LRU_file_names, LRU_file_sizes, LRU_hashes, lru_size, true)){
					FILE* get_file = fopen(file_name, "rb");
					if(get_file){
						write_OK(connfd, file_name);
						long int file_size = read_file_size(get_file);
						char *file_buffer = (char*)malloc(sizeof(char)*file_size);
						fread(file_buffer, file_size, 1, get_file);
						char* hashed_file = hash_MD5(file_buffer);
						write_size(connfd, file_size);
						write_hash(connfd, hashed_file);
						write_file(connfd, file_buffer, file_size);
						if(lru_size > 0){
							sprintf(LRU_file_names[lru_index], "%s", (file_name));
							LRU_file_sizes[lru_index] = file_size;
							LRU_hashes[lru_index] = hashed_file;
							LRU[lru_index] = file_buffer;
							lru_index++;
							if(lru_index == lru_size){
								*(&lru_index) = 0;
							}
						}
						// update_LRU(LRU_file_names, LRU_file_sizes, LRU_hashes, LRU, file_name, file_size, hashed_file, file_buffer, lru_index, lru_size);
					}
					else{
						fprintf(stderr, "GETC - File not found %s\n", file_name);
					}
					fclose(get_file);
				}
			}
			else{
				printf("Invalid Request");
			}


}

/*
 * main() - parse command line, create a socket, handle requests
 */
int main(int argc, char **argv)
{
	/* for getopt */
	long opt;
	int  lru_size = 10;
	int  port     = 9000;
	bool multithread = false;

	check_team(argv[0]);

	/* parse the command-line options.  They are 'p' for port number,  */
	/* and 'l' for lru cache size, 'm' for multi-threaded.  'h' is also supported. */
	while((opt = getopt(argc, argv, "hml:p:")) != -1)
	{
		switch(opt)
		{
		case 'h': help(argv[0]); break;
		case 'l': lru_size = atoi(argv[0]); break;
		case 'm': multithread = true;	break;
		case 'p': port = atoi(optarg); break;
		}
	}

	/* open a socket, and start handling requests */
	int fd = open_server_socket(port);
	handle_requests(fd, file_server, lru_size, multithread);

	exit(0);
}
