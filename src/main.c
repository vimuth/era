#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h> // Unbuntu 18 on x86_64 has this at /usr/include/x86_64-linux-gnu/sys/socket.h
#include <signal.h>
#include <fcntl.h> // for open
#include <unistd.h> // for close
#include <arpa/inet.h> // for inet_addr
#include <sys/time.h>

#include "globals.h"
#include "getopt.h"

#include "occgrid.h"    // Occupancy Grid Map Create/Fuse
#include "lz4.h"        // LZ4 Compression/Decompression
#include "xmit_pipe.h"  // IEEE 802.11p WiFi SDR Transmit Pipeline
#include "recv_pipe.h"  // IEEE 802.11p WiFi SDR Receive Pipeline

// The PORTS are defined in the compilation process, and comforms to the
// definition in the read_bag_x.py files and wifi_comm_x.py files.

char bag_inet_addr_str[20];
char wifi_inet_addr_str[20];
unsigned  max_time_steps = ~1;

int bag_sock = 0;
int xmit_sock = 0;
int recv_sock = 0;

char *ack = "OK";

float odometry[] = {0.0, 0.0, 0.0};

char pr_map_char[256];

// These variables capture "time" spent in various parts ofthe workload
#ifndef SKIP_TIMING
 struct timeval stop_prog, start_prog;

 struct timeval stop_proc_odo, start_proc_odo;
 uint64_t proc_odo_sec  = 0LL;
 uint64_t proc_odo_usec = 0LL;

 struct timeval stop_proc_lidar, start_proc_lidar;
 uint64_t proc_lidar_sec  = 0LL;
 uint64_t proc_lidar_usec = 0LL;

 struct timeval stop_proc_data, start_proc_data;
 uint64_t proc_data_sec  = 0LL;
 uint64_t proc_data_usec = 0LL;

 struct timeval stop_pd_cloud2grid, start_pd_cloud2grid;
 uint64_t pd_cloud2grid_sec  = 0LL;
 uint64_t pd_cloud2grid_usec = 0LL;

 struct timeval stop_pd_lz4_cmp, start_pd_lz4_cmp;
 uint64_t pd_lz4_cmp_sec  = 0LL;
 uint64_t pd_lz4_cmp_usec = 0LL;

 struct timeval stop_pd_xmit_pipe, start_pd_xmit_pipe;
 uint64_t pd_xmit_pipe_sec  = 0LL;
 uint64_t pd_xmit_pipe_usec = 0LL;

 struct timeval stop_pd_xmit_send, start_pd_xmit_send;
 uint64_t pd_xmit_send_sec  = 0LL;
 uint64_t pd_xmit_send_usec = 0LL;

 struct timeval stop_pd_xmit_recv, start_pd_xmit_recv;
 uint64_t pd_xmit_recv_sec  = 0LL;
 uint64_t pd_xmit_recv_usec = 0LL;

 struct timeval stop_pd_recv_pipe, start_pd_recv_pipe;
 uint64_t pd_recv_pipe_sec  = 0LL;
 uint64_t pd_recv_pipe_usec = 0LL;

struct timeval stop_pd_lz4_uncmp, start_pd_lz4_uncmp;
 uint64_t pd_lz4_uncmp_sec  = 0LL;
 uint64_t pd_lz4_uncmp_usec = 0LL;

 struct timeval stop_pd_combGrids, start_pd_combGrids;
 uint64_t pd_combGrids_sec  = 0LL;
 uint64_t pd_combGrids_usec = 0LL;

#endif

// Forward Declarations
void print_usage(char * pname);
void dump_final_run_statistics();
void INThandler(int dummy);
void closeout_and_exit(int rval);



// Functions, code, etc.
void print_usage(char * pname) {
  printf("Usage: %s <OPTIONS>\n", pname);
  printf(" OPTIONS:\n");
  printf("    -h         : print this helpful usage info\n");
  printf("    -B <str>   : set the internet-address for the bagfile server to <str>\n");
  printf("    -W <str>   : set the internet-address for the WiFi server to <str>\n");
  printf("    -s <Num>     : exit run after <Num> Lidar time-steps (msgs)\n");
}

void INThandler(int dummy)
{
  printf("In SIGINT INThandler -- Closing the connection and exiting\n");
  close(bag_sock);
  close(xmit_sock);
  close(recv_sock);
  exit(-1);
}


void closeout_and_exit(int rval)
{
  dump_final_run_statistics();
  printf("closeout_and_exit -- Closing the connection and exiting %d\n", rval);
  close(bag_sock);
  close(xmit_sock);
  close(recv_sock);
  exit(rval);
}

/* 
float bytes_to_float(unsigned char * bytes)
{
	unsigned char b[] = {bytes[0], bytes[1], bytes[2], bytes[3]};
	float f;
	memcpy(&f, &b, sizeof(f));

	return f;
}
*/

int counter = 0;

unsigned odo_count = 0;
unsigned lidar_count = 0;
unsigned xmit_recv_count = 0;

void write_array_to_file(unsigned char * data, long size)
{
  const int dimx = 50, dimy = 50;
  int i, j;

  char file_name[32];

  snprintf(file_name,sizeof(char)*32, "%s%04d.ppm", IMAGE_FN, counter);

  FILE *fp = fopen(file_name, "w");
  fprintf(fp, "P3 %d %d 255\n", dimx, dimy);

  for (j = 0; j < dimy*dimx; ++j) {
    fprintf(fp, " %d %d %d ", data[j], data[j], data[j]);
   }

  fclose(fp);
  counter++;
}

//#define MAX_GRID_SIZE   50 * 50  // Taken from size_x, size_y, resolution
#define MAX_UNCOMPRESSED_DATA_SIZE  sizeof(Costmap2D) // MAX_GRID_SIZE
#define MAX_COMPRESSED_DATA_SIZE    MAX_UNCOMPRESSED_DATA_SIZE //(In case of no compression)?  

#define MAX_XMIT_OUTPUTS  41800  // Really something like 41782 I think

int read_all(int sock, char* buffer, int xfer_in_bytes)
{
  char * ptr;
  int message_size = xfer_in_bytes;
  char* message_ptr = buffer;
  int total_recvd = 0;
  while(total_recvd < message_size) {
    unsigned rem_len = (message_size - total_recvd);
    int valread = read(sock , message_ptr, rem_len);
    message_ptr = message_ptr + valread;
    total_recvd += valread;
    DBGOUT2(printf("        read %d bytes for %d total bytes of %d\n", valread, total_recvd, message_size));
    if (valread == 0) {
      printf("  read_all got ZERO bytes -- END of TRANSFER?\n");
      return total_recvd;
    }
  }
  return total_recvd;
}


void process_data(char* data, int data_size)
{
        DBGOUT(printf("Calling cloudToOccgrid...\n"));
	int valread = 0;
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_cloud2grid, NULL);
       #endif	
	unsigned char * grid = cloudToOccgrid((float*)data, data_size/sizeof(float),
		odometry[0],odometry[1],odometry[2],1.5,
		false,
		0.05, 2.05,
		100,
	        100, 100, 2.0,  // size_x, size_y, resolution
		NO_INFORMATION);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_cloud2grid, NULL);
	pd_cloud2grid_sec   += stop_pd_cloud2grid.tv_sec  - start_pd_cloud2grid.tv_sec;
	pd_cloud2grid_usec  += stop_pd_cloud2grid.tv_usec - start_pd_cloud2grid.tv_usec;
       #endif

	// Write the read-in image to a file
	//write_array_to_file(grid, 100/2.0*100/2.0);

	// Now we compress the grid for transmission...
	Costmap2D* local_map = &(master_observation.master_costmap);
	DBGOUT(printf("Calling LZ4_compress_default...\n");
	       printf("  Input CostMAP: AV x %lf y %lf z %lf\n", local_map->av_x, local_map->av_y, local_map->av_z);
	       printf("               : Cell_Size %lf X-Dim %u Y-Dim %u\n", local_map->cell_size, local_map->x_dim, local_map->y_dim);
	       printf("  ");
	       for (int ii = 0; ii < 50; ii++) {
		 for (int ij = 0; ij < 50; ij++) {
		   int idx = 50*ii + ij;
		   printf("%c", pr_map_char[local_map->costmap_[idx]]);
		 }
		 printf("\n  ");
	       }
	       printf("\n"));
	
	unsigned char cmp_data[MAX_COMPRESSED_DATA_SIZE];
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_lz4_cmp, NULL);
       #endif	
	int n_cmp_bytes = LZ4_compress_default((char*)local_map, (char*)cmp_data, MAX_UNCOMPRESSED_DATA_SIZE, MAX_COMPRESSED_DATA_SIZE);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_lz4_cmp, NULL);
	pd_lz4_cmp_sec   += stop_pd_lz4_cmp.tv_sec  - start_pd_lz4_cmp.tv_sec;
	pd_lz4_cmp_usec  += stop_pd_lz4_cmp.tv_usec - start_pd_lz4_cmp.tv_usec;
       #endif
	DBGOUT(double c_ratio = 100*(1-((double)(n_cmp_bytes)/(double)(MAX_UNCOMPRESSED_DATA_SIZE)));
	       printf("  Back from LZ4_compress_default: %lu bytes -> %u bytes for %5.2f%%\n", MAX_UNCOMPRESSED_DATA_SIZE, n_cmp_bytes, c_ratio));

	// Now we encode and transmit the grid...
	DBGOUT(printf("Calling do_xmit_pipeline for %u compressed grid elements\n", n_cmp_bytes));
	int n_xmit_out;
	float xmit_out_real[MAX_XMIT_OUTPUTS];
	float xmit_out_imag[MAX_XMIT_OUTPUTS];
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_xmit_pipe, NULL);
       #endif	
	do_xmit_pipeline(n_cmp_bytes, cmp_data, &n_xmit_out, xmit_out_real, xmit_out_imag);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_xmit_pipe, NULL);
	pd_xmit_pipe_sec   += stop_pd_xmit_pipe.tv_sec  - start_pd_xmit_pipe.tv_sec;
	pd_xmit_pipe_usec  += stop_pd_xmit_pipe.tv_usec - start_pd_xmit_pipe.tv_usec;
       #endif
	DBGOUT(printf("  Back from do_xmit_pipeline with %u xmit outputs...\n", n_xmit_out));

	// This is now the content that should be sent out via IEEE 802.11p WiFi
	//  The n_xmit_out values of xmit_out_real and xmit_out_imag
	// Connect to the Wifi-Socket and send the n_xmit_out
	char w_buffer[10];
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_xmit_send, NULL);
       #endif	
	unsigned xfer_bytes = n_xmit_out*sizeof(float);
	snprintf(w_buffer, 9, "X%-6uX", xfer_bytes);
	DBGOUT(printf("\nXMIT Sending %s on XMIT port %u socket\n", w_buffer, XMIT_PORT));
	send(xmit_sock, w_buffer, 8, 0);
	DBGOUT(printf("     Send %u REAL values %u bytes on XMIT port %u socket\n", n_xmit_out, xfer_bytes, XMIT_PORT));
	DBGOUT2(printf("XFER %4u : Dumping XMIT-PIPE REAL raw bytes\n", xmit_recv_count);
		for (int i = 0; i < n_xmit_out; i++) {
		  printf("XFER %4u REAL-byte %6u : %f\n", xmit_recv_count, i, xmit_out_real[i]);
		}
		printf("\n"));
	send(xmit_sock, (char*)(xmit_out_real), n_xmit_out*sizeof(float), 0);
	DBGOUT(printf("     Send %u IMAG values %u bytes on XMIT port %u socket\n", n_xmit_out, xfer_bytes, XMIT_PORT));
	DBGOUT2(printf("XFER %4u : Dumping XMIT-PIPE IMAG raw bytes\n", xmit_recv_count);
		for (int i = 0; i < n_xmit_out; i++) {
		  printf("XFER %4u IMAG-byte %6u : %f\n", xmit_recv_count, i, xmit_out_imag[i]);
		}
	       printf("\n"));
	send(xmit_sock, (char*)(xmit_out_imag), n_xmit_out*sizeof(float), 0);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_xmit_send, NULL);
	pd_xmit_send_sec   += stop_pd_xmit_send.tv_sec  - start_pd_xmit_send.tv_sec;
	pd_xmit_send_usec  += stop_pd_xmit_send.tv_usec - start_pd_xmit_send.tv_usec;
       #endif

	// Now we take in a recevied transmission with the other AV's map
	// If we receive a transmission, the process to turn it back into the gridMap is:
	int   n_recvd_in;
	float recvd_in_real[MAX_XMIT_OUTPUTS];
	float recvd_in_imag[MAX_XMIT_OUTPUTS];

	DBGOUT(printf("\nTrying to Receive data on RECV port %u socket\n", RECV_PORT));
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_xmit_recv, NULL);
       #endif	
	valread = read_all(recv_sock, w_buffer, 8);
	DBGOUT2(printf("  RECV got msg %s\n", w_buffer);
		printf("  RECV msg psn %s\n", "01234567890"));
	if (valread < 8) {
	  if (valread == 0) {
	    printf("  RECV header got ZERO bytes -- END of TRANSFER?\n");
	    closeout_and_exit(-1);
	  } else {
	    printf("  RECV header got TOO FEW bytes %u vs %u : INTERRUPTED TRANSFER?\n", valread, 8);
	    closeout_and_exit(-1);
	  }
	}
	if(!(w_buffer[0] == 'X' && w_buffer[7] == 'X')) {
	  printf("ERROR: Unexpected message from WiFi...\n");
	  closeout_and_exit(-3);
	}

	char * ptr;
	unsigned xfer_in_bytes = strtol(w_buffer+1, &ptr, 10);
	n_recvd_in = xfer_in_bytes / sizeof(float);
	DBGOUT(printf("     Recv %u REAL values %u bytes from RECV port %u socket\n", n_recvd_in, xfer_in_bytes, RECV_PORT));
	valread = read_all(recv_sock, (char*)recvd_in_real, xfer_in_bytes);
	if (valread < xfer_in_bytes) {
	  if (valread == 0) {
	    printf("  RECV REAL got ZERO bytes -- END of TRANSFER?\n");
	    closeout_and_exit(-1);
	  } else {
	    printf("  RECV REAL got TOO FEW bytes %u vs %u : INTERRUPTED TRANSFER?\n", valread, xfer_in_bytes);
	    closeout_and_exit(-1);
	  }
	}
	DBGOUT2(printf("XFER %4u : Dumping RECV-PIPE REAL raw bytes\n", xmit_recv_count);
	       for (int i = 0; i < n_recvd_in; i++) {
		 printf("XFER %4u REAL-byte %6u : %f\n", odo_count, i, recvd_in_real[i]);
	       }
	       printf("\n"));

	DBGOUT(printf("     Recv %u IMAG values %u bytes from RECV port %u socket\n", n_recvd_in, xfer_in_bytes, RECV_PORT));
	valread = read_all(recv_sock, (char*)recvd_in_imag, xfer_in_bytes);
	if (valread < xfer_in_bytes) {
	  if (valread == 0) {
	    printf("  RECV IMAG got ZERO bytes -- END of TRANSFER?\n");
	    closeout_and_exit(-1);
	  } else {
	    printf("  RECV IMAG got TOO FEW bytes %u vs %u : INTERRUPTED TRANSFER?\n", valread, xfer_in_bytes);
	    closeout_and_exit(-1);
	  }
	}
	DBGOUT2(printf("XFER %4u : Dumping RECV-PIPE IMAG raw bytes\n", xmit_recv_count);
	       for (int i = 0; i < n_recvd_in; i++) {
		 printf("XFER %4u IMAG-byte %6u : %f\n", odo_count, i, recvd_in_imag[i]);
	       }
	       printf("\n"));
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_xmit_recv, NULL);
	pd_xmit_recv_sec   += stop_pd_xmit_recv.tv_sec  - start_pd_xmit_recv.tv_sec;
	pd_xmit_recv_usec  += stop_pd_xmit_recv.tv_usec - start_pd_xmit_recv.tv_usec;
       #endif

	// Now we have the tranmission input data to be decoded...
	DBGOUT(printf("Calling do_recv_pipeline...\n"));
	int   recvd_msg_len;
	unsigned char recvd_msg[1500]; // MAX size of original message in bytes
	// Fake this with a "loopback" of the xmit message..
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_recv_pipe, NULL);
       #endif	
	do_recv_pipeline(n_recvd_in, recvd_in_real, recvd_in_imag, &recvd_msg_len, recvd_msg);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_recv_pipe, NULL);
	pd_recv_pipe_sec   += stop_pd_recv_pipe.tv_sec  - start_pd_recv_pipe.tv_sec;
	pd_recv_pipe_usec  += stop_pd_recv_pipe.tv_usec - start_pd_recv_pipe.tv_usec;
       #endif
	//do_recv_pipeline(n_xmit_out, xmit_out_real, xmit_out_imag, &recvd_msg_len, recvd_msg);

	// Now we decompress the grid received via transmission...
	DBGOUT(printf("Calling LZ4_decompress_default...\n"));
	unsigned char uncmp_data[MAX_UNCOMPRESSED_DATA_SIZE];
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_lz4_uncmp, NULL);
       #endif	
	int dec_bytes = LZ4_decompress_safe((char*)recvd_msg, (char*)uncmp_data, n_cmp_bytes, MAX_UNCOMPRESSED_DATA_SIZE);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_lz4_uncmp, NULL);
	pd_lz4_uncmp_sec   += stop_pd_lz4_uncmp.tv_sec  - start_pd_lz4_uncmp.tv_sec;
	pd_lz4_uncmp_usec  += stop_pd_lz4_uncmp.tv_usec - start_pd_lz4_uncmp.tv_usec;
       #endif

	Costmap2D* remote_map = (Costmap2D*)&(uncmp_data); // Convert "type" to Costmap2D
	DBGOUT(printf("  Back from LZ4_decompress_safe with %u decompressed bytes\n", dec_bytes);
	       printf("  Remote CostMAP: AV x %lf y %lf z %lf\n", remote_map->av_x, remote_map->av_y, remote_map->av_z);
	       printf("                : Cell_Size %lf X-Dim %u Y-Dim %u\n", remote_map->cell_size, remote_map->x_dim, remote_map->y_dim);
	       printf("  ");
	       for (int ii = 0; ii < 50; ii++) {
		 for (int ij = 0; ij < 50; ij++) {
		   int idx = 50*ii + ij;
		   printf("%c", pr_map_char[remote_map->costmap_[idx]]);
		 }
		 printf("\n  ");
	       }
	       printf("\n"));
	
	// Then we should "Fuse" the received GridMap with our local one
	//  We need to "peel out" the remote odometry data from somewhere (in the message?)
	//unsigned char* combineGrids(unsigned char* grid1, unsigned char* grid2, double robot_x1, double robot_y1,
	DBGOUT(printf("\nCalling combineGrids...\n"));
	// Note: The direction in which this is called is slightly significant:
	//  The first map is copied into the second map, in this case remote into local,
	//  and the x_dim, et.c MUST correspond to that second map (here local)
       #ifndef SKIP_TIMING
	gettimeofday(&start_pd_combGrids, NULL);
       #endif	
	combineGrids(remote_map->costmap_, local_map->costmap_,
		     remote_map->av_x, remote_map->av_y,
		     local_map->av_x, local_map->av_y,
		     local_map->x_dim, local_map->y_dim, local_map->cell_size,
		     local_map->default_value);
       #ifndef SKIP_TIMING
	gettimeofday(&stop_pd_combGrids, NULL);
	pd_combGrids_sec   += stop_pd_combGrids.tv_sec  - start_pd_combGrids.tv_sec;
	pd_combGrids_usec  += stop_pd_combGrids.tv_usec - start_pd_combGrids.tv_usec;
       #endif
	DBGOUT(printf("  Fused CostMAP : AV x %lf y %lf z %lf\n", local_map->av_x, local_map->av_y, local_map->av_z);
	       printf("                : Cell_Size %lf X-Dim %u Y-Dim %u\n", local_map->cell_size, local_map->x_dim, local_map->y_dim);
	       printf("  ");
	       for (int ii = 0; ii < 50; ii++) {
		 for (int ij = 0; ij < 50; ij++) {
		   int idx = 50*ii + ij;
		   printf("%c", pr_map_char[local_map->costmap_[idx]]);
		 }
		 printf("\n  ");
	       }
	       printf("\n"));

	// Write the combined map to a file
	write_array_to_file(local_map->costmap_, 100/2.0*100/2.0);
	
	DBGOUT(printf("Returning from process_data\n"));
	fflush(stdout);
}


int main(int argc, char *argv[])
{
        struct sockaddr_in bag_servaddr;
	struct sockaddr_in xmit_servaddr;
	struct sockaddr_in recv_servaddr;
	unsigned char buffer[200002] = {0};

	snprintf(bag_inet_addr_str, 20, "127.0.0.1");
	snprintf(wifi_inet_addr_str, 20, "127.0.0.1");
	
	xmit_pipe_init(); // Initialize the IEEE SDR Transmit Pipeline
	
	signal(SIGINT, INThandler);

	// Set up the print-map-character array (to xlate map values to ASCII symbols)
	for (int i = 0; i < 256; i++) {
	  pr_map_char[i] = '?';
	}
	pr_map_char[NO_INFORMATION]  = '.';
	pr_map_char[FREE_SPACE]      = ' ';
	pr_map_char[LETHAL_OBSTACLE] = 'X';

	// Use getot to read in run-time options
	// put ':' in the starting of the
	// string so that program can
	// distinguish between '?' and ':'
	int opt;
	while((opt = getopt(argc, argv, ":hB:W:s:")) != -1) {
		switch(opt) {
		case 'h':
			print_usage(argv[0]);
			exit(0);
		case 'B':
			snprintf(bag_inet_addr_str, 20, "%s", optarg);
			break;
		case 'W':
			snprintf(wifi_inet_addr_str, 20, "%s", optarg);
			break;
		case 's':
		        max_time_steps = atoi(optarg);
			DBGOUT(printf("Set maximum time steps to %u\n", max_time_steps));
			break;

		case ':':
			printf("option needs a value\n");
			break;
		case '?':
			printf("unknown option: %c\n", optopt);
			break;
		}
	}

	if (max_time_steps != ~1) {
	  printf("Limiting to %u max time steps\n", max_time_steps);
	} else {
	  printf("Running the entire bag file.\n");
	}

	printf("Connecting to bag-server at IP %s PORT %u\n", bag_inet_addr_str, BAG_PORT);
	// Open and connect to the BAG_SERVER 
	if ((bag_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)  {
	  printf("BAG Socket creation failed...\n");
	  exit(0);
	}
	else {
	  printf("BAG Socket successfully created..\n");
	}

	bag_servaddr.sin_family = AF_INET;
	bag_servaddr.sin_addr.s_addr = inet_addr(bag_inet_addr_str);
	bag_servaddr.sin_port = htons(BAG_PORT);

	while (true) {
	  if (connect(bag_sock, (struct sockaddr*)&bag_servaddr, sizeof(bag_servaddr)) != 0) {
	    printf("connection with the BAG server failed...\n");
	    sleep(1);
	    continue;
	  }
	  else {
	    printf("connected to the BAG server..\n");
	    break;
	  }
	}

	// Open and connect to the XMIT_SERVER
	printf("Connecting to xmit-server at IP %s PORT %u\n", wifi_inet_addr_str, XMIT_PORT);
	if ((xmit_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)  {
	  printf("WIFI XMIT Socket creation failed...\n");
	  exit(0);
	}
	else {
	  printf("WIFI XMIT Socket successfully created..\n");
	}

	xmit_servaddr.sin_family = AF_INET;
	xmit_servaddr.sin_addr.s_addr = inet_addr(wifi_inet_addr_str);
	xmit_servaddr.sin_port = htons(XMIT_PORT);

	while (true) {
	  if (connect(xmit_sock, (struct sockaddr*)&xmit_servaddr, sizeof(xmit_servaddr)) != 0) {
	    printf("connection with the WIFI XMIT server failed...\n");
	    sleep(1);
	    continue;
	  }
	  else {
	    printf("connected to the WIFI XMIT server..\n");
	    break;
	  }
	}

	// Open and connect to the RECV_SERVER 
	printf("Connecting to recv-server at IP %s PORT %u\n", wifi_inet_addr_str, RECV_PORT);
	if ((recv_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)  {
	  printf("WIFI RECV Socket creation failed...\n");
	  exit(0);
	}
	else {
	  printf("WIFI RECV Socket successfully created..\n");
	}

	recv_servaddr.sin_family = AF_INET;
	recv_servaddr.sin_addr.s_addr = inet_addr(wifi_inet_addr_str); //"127.0.0.1");
	recv_servaddr.sin_port = htons(RECV_PORT);

	while (true) {
	  if (connect(recv_sock, (struct sockaddr*)&recv_servaddr, sizeof(recv_servaddr)) != 0) {
	    printf("connection with the WIFI RECV server failed...\n");
	    sleep(1);
	    continue;
	  }
	  else {
	    printf("connected to the WIFI RECV server..\n");
	    break;
	  }
	}

       #ifndef SKIP_TIMING
	gettimeofday(&start_prog, NULL);
       #endif
	bool hit_eof = false;
	while ((!hit_eof) && (lidar_count < max_time_steps)) {
	        DBGOUT(printf("Calling read_all on the BAG socket...\n"); fflush(stdout));
		//int valread = read(bag_sock , buffer, 10);
		int valread = read_all(bag_sock, buffer, 10);
		DBGOUT(printf("Top: read %d bytes\n", valread));
		if (valread < 10) {
		  if (valread == 0) {
		    // Zero bytes out implies we hit EOF
		    hit_eof = true;
		  } else {
		    printf("  TOO FEW bytes %u vs %u : INTERRUPTED TRANSFER?\n", valread, 10);
		    closeout_and_exit(-1);
		  }
		}

		if(buffer[0] == 'L' && buffer[9] == 'L') {
                       #ifndef SKIP_TIMING
		         gettimeofday(&start_proc_lidar, NULL);
                       #endif
			char * ptr;
			int message_size = strtol(buffer+1, &ptr, 10);
			DBGOUT(printf("Lidar: expecting message size: %d\n", message_size));
			send(bag_sock, ack, 2, 0);

			int total_bytes_read = read_all(bag_sock, buffer, message_size);
			if (total_bytes_read < message_size) {
			  if (total_bytes_read == 0) {
			    printf("  Lidar read got ZERO bytes -- END of TRANSFER?\n");
			    closeout_and_exit(-1);
			  } else {
			    printf("  Lidar read got TOO FEW bytes %u vs %u : INTERRUPTED TRANSFER?\n", total_bytes_read, message_size);
			    closeout_and_exit(-1);
			  }
			}
			if (total_bytes_read > message_size) {
                          printf("NOTE: read more total bytes than expected: %u vs %u\n", total_bytes_read, message_size);
                        }
			DBGOUT(printf("Calling process_data for %d total bytes\n", total_bytes_read));
			printf("Processing Lidar msg %4u data\n", lidar_count);
                       #ifndef SKIP_TIMING
		         gettimeofday(&start_proc_data, NULL);
                       #endif
			process_data(buffer, total_bytes_read);
			DBGOUT(printf("Back from process_data for Lidar\n"); fflush(stdout));
			lidar_count++;
                       #ifndef SKIP_TIMING
		         gettimeofday(&stop_proc_lidar, NULL);
			 proc_data_sec   += stop_proc_lidar.tv_sec  - start_proc_data.tv_sec;
			 proc_data_usec  += stop_proc_lidar.tv_usec - start_proc_data.tv_usec;
			 proc_lidar_sec  += stop_proc_lidar.tv_sec  - start_proc_lidar.tv_sec;
			 proc_lidar_usec += stop_proc_lidar.tv_usec - start_proc_lidar.tv_usec;
                       #endif
		}
		else if(buffer[0] == 'O' && buffer[9] == 'O') {
                       #ifndef SKIP_TIMING
		         gettimeofday(&start_proc_odo, NULL);
                       #endif
			char * ptr;
			int message_size = strtol(buffer+1, &ptr, 10);
			DBGOUT(printf("Odometry: expecting message size: %d\n", message_size));
			send(bag_sock, ack, 2, 0);

			int total_bytes_read = 0;

			//valread = read(bag_sock , buffer, 10000);
			valread = read_all(bag_sock , buffer, message_size);
			DBGOUT(printf("read %d bytes\n", valread));
			if (valread < message_size) {
			  if (valread == 0) {
			    printf("  Odo read got ZERO bytes -- END of TRANSFER?\n");
			    closeout_and_exit(-1);
			  } else {
			    printf("  Odo read got TOO FEW bytes %u vs %u : INTERRUPTED TRANSFER?\n", valread, message_size);
			    closeout_and_exit(-1);
			  }
			}

			odometry[0] = *((float*)(buffer));   //bytes_to_float(buffer);
			odometry[1] = *((float*)(buffer+4)); //bytes_to_float(buffer+4);
			odometry[2] = *((float*)(buffer+8)); //bytes_to_float(buffer+8);

			printf("Odometry msg %4u: %.2f %.2f %.2f\n", odo_count, odometry[0], odometry[1], odometry[2]);
			odo_count++;
                       #ifndef SKIP_TIMING
		         gettimeofday(&stop_proc_odo, NULL);
			 proc_odo_sec  += stop_proc_odo.tv_sec  - start_proc_odo.tv_sec;
			 proc_odo_usec += stop_proc_odo.tv_usec - start_proc_odo.tv_usec;
                       #endif

		} else {
		  /*DBGOUT(printf("BUFFER : '");
			 for (int ii = 0; ii < 8; ii++) {
			   printf("%c", buffer[ii]);
			 }
			 printf("'\n");
			 fflush(stdout));*/
		}

	}

	dump_final_run_statistics();
	close(bag_sock);
	close(xmit_sock);
	close(recv_sock);
}





void dump_final_run_statistics()
{
 #ifndef SKIP_TIMING
  gettimeofday(&stop_prog, NULL);
  uint64_t total_exec = (uint64_t) (stop_prog.tv_sec - start_prog.tv_sec) * 1000000 + (uint64_t) (stop_prog.tv_usec - start_prog.tv_usec);
  uint64_t proc_odo   = (uint64_t) (proc_odo_sec)  * 1000000 + (uint64_t) (proc_odo_usec);
  uint64_t proc_lidar   = (uint64_t) (proc_lidar_sec)  * 1000000 + (uint64_t) (proc_lidar_usec);
  uint64_t proc_data   = (uint64_t) (proc_data_sec)  * 1000000 + (uint64_t) (proc_data_usec);

  uint64_t pd_cloud2grid   = (uint64_t) (pd_cloud2grid_sec)  * 1000000 + (uint64_t) (pd_cloud2grid_usec);
  uint64_t pd_lz4_cmp   = (uint64_t) (pd_lz4_cmp_sec)  * 1000000 + (uint64_t) (pd_lz4_cmp_usec);
  uint64_t pd_xmit_pipe   = (uint64_t) (pd_xmit_pipe_sec)  * 1000000 + (uint64_t) (pd_xmit_pipe_usec);
  uint64_t pd_xmit_send   = (uint64_t) (pd_xmit_send_sec)  * 1000000 + (uint64_t) (pd_xmit_send_usec);
  uint64_t pd_xmit_recv   = (uint64_t) (pd_xmit_recv_sec)  * 1000000 + (uint64_t) (pd_xmit_recv_usec);
  uint64_t pd_recv_pipe   = (uint64_t) (pd_recv_pipe_sec)  * 1000000 + (uint64_t) (pd_recv_pipe_usec);
  uint64_t pd_lz4_uncmp   = (uint64_t) (pd_lz4_uncmp_sec)  * 1000000 + (uint64_t) (pd_lz4_uncmp_usec);
  uint64_t pd_combGrids   = (uint64_t) (pd_combGrids_sec)  * 1000000 + (uint64_t) (pd_combGrids_usec);
#endif

  printf("\nFinal Run Statistics for %u total Lidar Time-Steps\n", lidar_count);
  printf("Timing (in usec):\n");
  printf(" Total workload main-loop : %10lu usec\n", total_exec);
  printf("   Total proc Odometry    :   %10lu usec\n", proc_odo);
  printf("   Total proc Lidar       :   %10lu usec\n", proc_lidar);
  printf("   Total proc Data        :   %10lu usec\n", proc_data);
  printf("     Total pd cloud2grid  :     %10lu usec\n", pd_cloud2grid);
  printf("     Total pd lz4_cmp     :     %10lu usec\n", pd_lz4_cmp);
  printf("     Total pd xmit_pipe   :     %10lu usec\n", pd_xmit_pipe);
  printf("     Total pd xmit_send   :     %10lu usec\n", pd_xmit_send);
  printf("     Total pd xmit_recv   :     %10lu usec\n", pd_xmit_recv);
  printf("     Total pd recv_pipe   :     %10lu usec\n", pd_recv_pipe);
  printf("     Total pd lz4_uncmp   :     %10lu usec\n", pd_lz4_uncmp);
  printf("     Total pd combGrids   :     %10lu usec\n", pd_combGrids);
  //printf("     Total pd Z  :      %10lu usec\n", pd_Z);
  printf("\nDone with the run...\n");
}
