#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
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
#include "Client.h"

void help(char *progname)
{
	printf("Usage: %s [OPTIONS]\n", progname);
	printf("Perform a PUT or a GET from a network file server\n");
	printf("  -P    PUT file indicated by parameter\n");
	printf("  -G    GET file indicated by parameter\n");
	printf("  -s    server info (IP or hostname)\n");
	printf("  -p    port on which to contact server\n");
	printf("  -S    for GETs, name to use when saving file locally\n");
}

void die(const char *msg1, const char *msg2)
{
	fprintf(stderr, "%s, %s\n", msg1, msg2);
	exit(0);
}

/*
 * connect_to_server() - open a connection to the server specified by the
 *                       parameters
 */
int connect_to_server(char *server, int port)
{
	int clientfd;
	struct hostent *hp;
	struct sockaddr_in serveraddr;
	char errbuf[256];                                   /* for errors */

	/* create a socket */
	if((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		die("Error creating socket: ", strerror(errno));
	}

	/* Fill in the server's IP address and port */
	if((hp = gethostbyname(server)) == NULL)
	{
		sprintf(errbuf, "%d", h_errno);
		die("DNS error: DNS error ", errbuf);
	}
	bzero((char *) &serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	bcopy((char *)hp->h_addr_list[0], (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
	serveraddr.sin_port = htons(port);

	/* connect */
	if(connect(clientfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0)
	{
		die("Error connecting: ", strerror(errno));
	}
	return clientfd;
}

/*
 * echo_client() - this is dummy code to show how to read and write on a
 *                 socket when there can be short counts.  The code
 *                 implements an "echo" client.
 */
void echo_client(int fd)
{
	// main loop
	while(1)
	{
		/* set up a buffer, clear it, and read keyboard input */
		const int MAXLINE = 8192;
		char buf[MAXLINE];
		bzero(buf, MAXLINE);
		if(fgets(buf, MAXLINE, stdin) == NULL)
		{
			if(ferror(stdin))
			{
				die("fgets error", strerror(errno));
			}
			break;
		}

		/* send keystrokes to the server, handling short counts */
		size_t n = strlen(buf);
		size_t nremain = n;
		ssize_t nsofar;
		char *bufp = buf;
		while(nremain > 0)
		{
			if((nsofar = write(fd, bufp, nremain)) <= 0)
			{
				if(errno != EINTR)
				{
					fprintf(stderr, "Write error: %s\n", strerror(errno));
					exit(0);
				}
				nsofar = 0;
			}
			nremain -= nsofar;
			bufp += nsofar;
		}

		/* read input back from socket (again, handle short counts)*/
		bzero(buf, MAXLINE);
		bufp = buf;
		nremain = MAXLINE;
		while(1)
		{
			if((nsofar = read(fd, bufp, nremain)) < 0)
			{
				if(errno != EINTR)
				{
					die("read error: ", strerror(errno));
				}
				continue;
			}
			/* in echo, server should never EOF */
			if(nsofar == 0)
			{
				die("Server error: ", "received EOF");
			}
			bufp += nsofar;
			nremain -= nsofar;
			if(*(bufp-1) == '\n')
			{
				*bufp = 0;
				break;
			}
		}

		/* output the result */
		printf("%s", buf);
	}
}

bool receive(int fd, char* buffer, size_t packet_size){
	size_t total_bytes_received = 0;
	size_t recent_size = 0;
	while(total_bytes_received != packet_size){
		total_bytes_received += (recent_size = read(fd, buffer, packet_size));
		if(recent_size < 0){
			//throw error
			return false;
		}
		buffer += recent_size;
	}
	return true;
}

void send(int fd, const char* buffer, size_t request_size){
	size_t total_bytes_sent = 0;
	size_t recent_size = 0;
	while(total_bytes_sent != request_size){
		total_bytes_sent += (recent_size = write(fd, buffer, request_size));
		if(recent_size < 0){
			//throw error
		}
		buffer += recent_size;
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

/*
 * get_file() - get a file from the server accessible via the given socket
 *              fd, and save it according to the save_name
 */
void get_file(int fd, char *get_name, char *save_name, bool checksum)
{
	if(!save_name){
		save_name = get_name;
	}

	if(checksum){
		const unsigned int request_size = 5 + strlen(get_name);
		char get_request[request_size];
		bzero(get_request, request_size);
		sprintf(get_request, "GETC %s\n", get_name);
		send(fd, get_request, request_size);
	}
	else{
		const unsigned int request_size = 4 + strlen(get_name);
		char get_request[request_size];
		bzero(get_request, request_size);
		sprintf(get_request, "GET %s\n", get_name);
		send(fd, get_request, request_size);
	}

	char OK_response[4+strlen(get_name)+1];
	if(read(fd, &OK_response, sizeof(OK_response)) < 0){
		perror("Inavlid OK - response from server");
	}
	else{
		long file_size;
		if(read(fd, &file_size, sizeof(file_size)) < 0){
			perror("Bad file size");
		}
		else{
			if(checksum){
				char* received_hash = (char *)malloc(2*MD5_DIGEST_LENGTH*sizeof(char));
				read(fd, received_hash, 32);
				char file_buffer[file_size+1];
				read(fd, file_buffer, file_size);
				file_buffer[file_size] = '\0';
				char* calculated_hash = hash_MD5(file_buffer);
				if(!strncmp(received_hash, calculated_hash, 32)){
					FILE *get_file = fopen(save_name, "wb");
					fwrite(file_buffer, file_size, 1, get_file);
					fclose(get_file);
				}
				else{
					perror("MD5 checksum invalid");
				}
			}
			else{
				char* file_buffer = (char *)malloc(file_size);
				read(fd, file_buffer, file_size);
				FILE *get_file = fopen(save_name, "wb");
				fwrite(file_buffer, file_size, 1, get_file);
				fclose(get_file);
			}
		}
	}
}

/*
 * put_file() - send a file to the server accessible via the given socket fd
 */
void put_file(int fd, char *put_name, bool checksum)
{
	if(!put_name){
		perror("No put name specified");
		exit(0);
	}
	FILE* put_file = fopen(put_name, "rb");
	if(put_file){

		fseek(put_file, 0, SEEK_END);
		long put_file_size = ftell(put_file);
		fseek(put_file, 0, SEEK_SET);

		char put_buffer[put_file_size];
		bzero(put_buffer, put_file_size);
		fread(put_buffer, put_file_size, 1, put_file);

		char* put_buffer_pointer = put_buffer;

		if(checksum){
			const unsigned long long request_size = 4+strlen(put_name)+1+33+sizeof(put_file_size)+1+put_file_size+1;

			char request_buffer[request_size];
			bzero(request_buffer, request_size);
			sprintf(request_buffer, "PUTC %s\n%s\n%ld\n%s\n", put_name, hash_MD5(put_buffer_pointer), put_file_size, put_buffer);

			if(write(fd, request_buffer, request_size) < 0){
				perror("Error writing file to server");
			}
		}
		else{
			const unsigned long long request_size = 4+strlen(put_name)+1+sizeof(put_file_size)+1+put_file_size+1;
			char request_buffer[request_size];
			bzero(request_buffer, request_size);
			sprintf(request_buffer, "PUT %s\n%ld\n%s\n", put_name, put_file_size, put_buffer);
			if(write(fd, request_buffer, request_size) < 0){
				perror("Error writing file to server");
			}
		}
	}
	else{
		perror("Invalid File");
	}
}

/*
 * main() - parse command line, open a socket, transfer a file
 */
int main(int argc, char **argv)
{
	/* for getopt */
	long  opt;
	char *server = NULL;
	char *put_name = NULL;
	char *get_name = NULL;
	int   port;
	char *save_name = NULL;
	bool checksum = false;

	check_team(argv[0]);

	/* parse the command-line options. */
	while((opt = getopt(argc, argv, "hs:P:G:S:p:c")) != -1)
	{
		switch(opt)
		{
			case 'h': help(argv[0]); break;
			case 's': server = optarg; break;
			case 'P': put_name = optarg; break;
			case 'G': get_name = optarg; break;
			case 'S': save_name = optarg; break;
			case 'p': port = atoi(optarg); break;
			case 'c': checksum = true;
		}
	}

	/* open a connection to the server */
	int fd = connect_to_server(server, port);

	/* put or get, as appropriate */
	if(put_name)
	{
		put_file(fd, put_name, checksum);
	}
	else
	{
		get_file(fd, get_name, save_name, checksum);
	}

	/* close the socket */
	int rc;
	if((rc = close(fd)) < 0)
	{
		die("Close error: ", strerror(errno));
	}
	exit(0);
}
