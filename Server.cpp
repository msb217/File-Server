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
#include <thread>
#include <mutex>
using namespace std;
mutex server_mtx;

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
		if(multithread){
			if(!fork()){
				service_function(connfd, param);
      }
			if(close(connfd) < 0){
				die("Error in close(): ", strerror(errno));
			}
		}
		else{
			service_function(connfd, param);
			/* clean up, await new connection */
			if(close(connfd) < 0)
			{
				die("Error in close(): ", strerror(errno));
			}
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
	server_mtx.lock();
	int OK_response_size = 3 + strlen(file_name) + 1;
	char OK_response[OK_response_size];
	sprintf(OK_response, "OK %s\n", file_name);
	if(write(connfd, OK_response, OK_response_size) < 0){
		fprintf(stderr, "%s", "Error writing OK\n");
		server_mtx.unlock();
		return false;
	}
	server_mtx.unlock();
	return true;
}

bool write_size(int connfd, long int file_size){
	server_mtx.lock();
	if(write(connfd, &file_size, sizeof(file_size)) < 0){
		fprintf(stderr, "%s", "Error writing file size\n");
		server_mtx.unlock();
		return false;
	}
	server_mtx.unlock();
	return true;
}

bool write_hash(int connfd, char* hashed_file){
	server_mtx.lock();
	if(write(connfd, hashed_file, 32) < 0){
		fprintf(stderr, "%s", "Error writing file hash\n");
		server_mtx.unlock();
		return false;
	}
	server_mtx.unlock();
	return true;
}

bool write_file(int connfd, char* file, long file_size){
	server_mtx.lock();
	if(write(connfd, file, file_size) < 0){
		fprintf(stderr, "%s", "Error writing file contents\n");
		server_mtx.unlock();
		return false;
	}
	server_mtx.unlock();
	return true;
}

long int read_file_size(FILE* file){
	server_mtx.lock();
	fseek(file, 0, SEEK_END);
	long int file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	server_mtx.unlock();
	return file_size;
}


bool get_cached(int connfd, char* file_name, char** LRU, char** LRU_file_names, long int* LRU_file_sizes, char** LRU_hashes, int lru_size, bool checksum){
		for(int i = 0; i < lru_size; i++){
			if(LRU_file_names[i]){
				if(!strncmp(LRU_file_names[i], file_name, strlen(file_name))){
					if(write_OK(connfd, file_name)){
						if(write_size(connfd, LRU_file_sizes[i])){
							if(checksum){
								if(write_hash(connfd, LRU_hashes[i])){
									if(write_file(connfd, LRU[i], LRU_file_sizes[i])){
										printf("Cached\n");
										return true;
									}
								}
							}
							else{
								if(write_file(connfd, LRU[i], LRU_file_sizes[i])){
									printf("Cached\n");
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

bool put_cached(char* file_name, char** LRU, char** LRU_file_names, long int* LRU_file_sizes,
	char** LRU_hashes, int lru_size, bool checksum, long int file_size, char* hash,
	char* file_buffer, FILE* file){
		for(int i = 0; i < lru_size; i++){
			if(LRU_file_names[i]){
				if(!strncmp(LRU_file_names[i], file_name, strlen(file_name))){
						if(file_size == LRU_file_sizes[i]){
							if(checksum){
								if(!strncmp(LRU_hashes[i], hash, strlen(LRU_hashes[i]))){
									if(!strncmp(LRU[i], file_buffer, strlen(LRU[i]))){
										printf("Cached\n");
										return true;
									}
									else{
										sprintf(LRU_file_names[i], "%s", (file_name));
										LRU_file_sizes[i] = file_size;
										LRU_hashes[i] = hash;
										LRU[i] = file_buffer;
										return true;
									}
								}
								else{
									sprintf(LRU_file_names[i], "%s", (file_name));
									LRU_file_sizes[i] = file_size;
									LRU_hashes[i] = hash;
									LRU[i] = file_buffer;
									return true;
								}
							}
							else{
								if(!strncmp(LRU[i], file_buffer, strlen(LRU[i]))){
									printf("Cached\n");
									return true;
								}
								else{
									sprintf(LRU_file_names[i], "%s", (file_name));
									LRU_file_sizes[i] = file_size;
									LRU_hashes[i] = hash;
									LRU[i] = file_buffer;
									return true;
								}
							}
						}
						else{
							sprintf(LRU_file_names[i], "%s", (file_name));
							LRU_file_sizes[i] = file_size;
							LRU_hashes[i] = hash;
							LRU[i] = file_buffer;
							return true;
						}
					}
				}
			}
	return false;
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
		*/

		static char **LRU = (char**)malloc(sizeof(char *)*lru_size);
		static char **LRU_file_names = (char**)malloc(sizeof(char *)*lru_size);
		static long int* LRU_file_sizes = (long int*)malloc(sizeof(long int)*lru_size);
		static char** LRU_hashes = (char**)malloc(sizeof(char *)*lru_size);
	 	static int lru_index = 0;
		static bool lru_initialized = false;
		static mutex cache_mtx;

		if(!lru_initialized){
			cache_mtx.lock();
			for(int i = 0; i < lru_size; i++){
				LRU[i] = (char*)malloc(MAXLINE * sizeof(char));
				LRU_file_names[i] = (char*)malloc(MAXLINE * sizeof(char));
				LRU_file_sizes[i] = 0;
				LRU_hashes[i] = (char*)malloc(MAXLINE * sizeof(char));
			}
			lru_initialized = true;
			cache_mtx.unlock();
		}

		/*
			Read the request from the given socket
		*/
		char      buf[MAXLINE];
		bzero(buf, MAXLINE);
		server_mtx.lock();
		read(connfd, buf, sizeof(buf));
		server_mtx.unlock();

		if(!strncmp(buf, "GET ", 4)){
			char* moving_buffer = buf;
			moving_buffer+=4;
			char* file_name = strtok(moving_buffer, "\n");

			/*
				If the file isn't cached the code within the loop is run - otherwise
				refere to get_cached to see how the Server responds with the cached contents
			*/
			cache_mtx.lock();
			if(!get_cached(connfd, file_name, LRU, LRU_file_names, LRU_file_sizes, LRU_hashes, lru_size, false)){
				cache_mtx.unlock();
				FILE* get_file = fopen(file_name, "rb");
				if(get_file){
					write_OK(connfd, file_name);

					long int file_size = read_file_size(get_file);

					char* file_buffer = (char*)malloc(sizeof(char)*file_size);
					fread(file_buffer, file_size, 1, get_file);
					char* hashed_file = hash_MD5(file_buffer);

					write_size(connfd, file_size);

					write_file(connfd, file_buffer, file_size);

					/*
						By allowing the lru_index to be incremnted prior to accessing the
						cache itself, we allow other threads to simulataneously work on
						the cache with the original thread
					*/
					if(lru_size > 0){
						cache_mtx.lock();
						int temp_index = lru_index;
						lru_index++;
						if(lru_index == lru_size){
							*(&lru_index) = 0;
						}
						cache_mtx.unlock();
						cache_mtx.lock();
						sprintf(LRU_file_names[temp_index], "%s", (file_name));
						LRU_file_sizes[temp_index] = file_size;
						LRU_hashes[temp_index] = hashed_file;
						LRU[temp_index] = file_buffer;
						cache_mtx.unlock();
					}
				}
				else{
					fprintf(stderr, "GET - File not found %s\n", file_name);
				}
				fclose(get_file);
			}
			else{
				cache_mtx.unlock();
			}
		}
		else if (!strncmp(buf, "GETC ", 5)){
			char* moving_buffer = buf;
			moving_buffer+=5;
			char* file_name = strtok(moving_buffer, "\n");
			cache_mtx.lock();
			if(!get_cached(connfd, file_name, LRU, LRU_file_names, LRU_file_sizes, LRU_hashes, lru_size, true)){
				cache_mtx.unlock();
					FILE* get_file = fopen(file_name, "rb");
					if(get_file){

						write_OK(connfd, file_name);

						long int file_size = read_file_size(get_file);

						char *file_buffer = (char*)malloc(sizeof(char)*file_size);
						fread(file_buffer, file_size, 1, get_file);
						char* hashed_file = hash_MD5(file_buffer);

						write_size(connfd, file_size);

						/*
							Same as GET except for this function
						*/
						write_hash(connfd, hashed_file);

						write_file(connfd, file_buffer, file_size);

						if(lru_size > 0){
							cache_mtx.lock();
							int temp_index = lru_index;
							lru_index++;
							if(lru_index == lru_size){
								*(&lru_index) = 0;
							}
							cache_mtx.unlock();
							cache_mtx.lock();
							sprintf(LRU_file_names[temp_index], "%s", (file_name));
							LRU_file_sizes[temp_index] = file_size;
							LRU_hashes[temp_index] = hashed_file;
							LRU[temp_index] = file_buffer;
							cache_mtx.unlock();
						}
						fclose(get_file);
					}
					else{
						fprintf(stderr, "GETC - File not found %s\n", file_name);
					}
				}
				else{
					cache_mtx.unlock();
				}
			}
			else if(!strncmp(buf, "PUT ", 4)){
				server_mtx.lock();
				char* moving_buffer = buf;
				moving_buffer+=4;
				char* file_name = strtok(moving_buffer, "\n");
				FILE* put_file = fopen(file_name, "wb");
				if(put_file){
					moving_buffer += strlen(file_name) + 1;
					char* file_size_string = strtok(moving_buffer, "\n");
					long int file_size = atoi(file_size_string);
					moving_buffer += strlen(file_size_string) + 1;
					char* file_contents = (char*)malloc((file_size+1)*sizeof(char));
					strncpy(file_contents, moving_buffer, file_size);
					file_contents[file_size] = '\0';
					char* hash = hash_MD5(file_contents);
					fwrite(file_contents, file_size, 1, put_file);
					cache_mtx.lock();
					if(!put_cached(file_name, LRU, LRU_file_names, LRU_file_sizes, LRU_hashes,
						lru_size, false, file_size, hash, file_contents, put_file)){
						cache_mtx.unlock();
						if(lru_size > 0){
							cache_mtx.lock();
							int temp_index = lru_index;
							lru_index++;
							if(lru_index == lru_size){
								*(&lru_index) = 0;
							}
							cache_mtx.unlock();
							cache_mtx.lock();
							sprintf(LRU_file_names[temp_index], "%s", (file_name));
							LRU_file_sizes[temp_index] = file_size;
							LRU_hashes[temp_index] = hash;
							LRU[temp_index] = file_contents;
							cache_mtx.unlock();
						}
					}
					else{
						cache_mtx.unlock();
					}
				}
				else{
					perror("Error opening file for writing");
				}
				fclose(put_file);
				server_mtx.unlock();
			}
			else if(!strncmp(buf, "PUTC ", 5)){
				server_mtx.lock();
				char* moving_buffer = buf;
				moving_buffer+=5;
				char* file_name = strtok(moving_buffer, "\n");
				moving_buffer += strlen(file_name) + 1;
				FILE* put_file = fopen(file_name, "wb");
				if(put_file){
					char* file_size_string = strtok(moving_buffer, "\n");
					long int file_size = atoi(file_size_string);
					moving_buffer += strlen(file_size_string) + 1;
					char* MD5_digest = strtok(moving_buffer, "\n");
					moving_buffer += 33;
					char* file_contents = (char*)malloc((file_size+1)*sizeof(char));
					strncpy(file_contents, moving_buffer, file_size);
					file_contents[file_size] = '\0';
					char* hash = hash_MD5(file_contents);
					if(!strncmp(hash, MD5_digest, 32)){
						fwrite(file_contents, file_size, 1, put_file);
						cache_mtx.lock();
						if(!put_cached(file_name, LRU, LRU_file_names, LRU_file_sizes, LRU_hashes,
							lru_size, false, file_size, hash, file_contents, put_file)){
							cache_mtx.unlock();
							if(lru_size > 0){
								cache_mtx.lock();
								int temp_index = lru_index;
								lru_index++;
								if(lru_index == lru_size){
									*(&lru_index) = 0;
								}
								cache_mtx.unlock();
								cache_mtx.lock();
								sprintf(LRU_file_names[temp_index], "%s", (file_name));
								LRU_file_sizes[temp_index] = file_size;
								LRU_hashes[temp_index] = hash;
								LRU[temp_index] = file_contents;
								cache_mtx.unlock();
							}
						}
						else{
							cache_mtx.unlock();
						}
					}
					else{
						perror("MD5 does not match");
					}
					fclose(put_file);
				}
				server_mtx.unlock();
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
