/*
 * Service Discovery Helper
 * For game server discovery across VLANs, and similar applications
 *
 * (c) Chris "SirSquidness" Holman, October 2013
 * Licensed under the MIT License license.
 * See LICENSE for more details
 *
 * Forwards UDP broadcasts on given ports out other interfaces on the system. 
 * Useful for discovering game (or other) servers on other VLANs. Makes sure 
 * not to send a broadcast back out the same interface. 
 *
 * Usage: 
 *  sudo ./sdh-proxy -a -p ports
 *  sudo ./sdh-proxy -i interfaces -p ports
 *  sudo ./sdh-proxy -i interfaces -p ports -d
 *  ./sdh-proxy -h
 *
 * Currently requires configuration to be hard coded. I'll fix that soon. 
 * (honest!). Edit the iface_list array to contain all interfaces you use.
 * Edit the filter_string example to include all UDP port numbers. 
 *
 * Requires root
 * Requires libpcap and the libpcap headers (libpcap-dev) to be installed.
 * 
 * Compile using:
 * gcc -g -std=gnu99 -o sdh-proxy sdh-proxy.c -lpcap -lpthread
 * (Makefile with this in it provided)
 */
#include <arpa/inet.h>
#include "timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pcap.h>
#include <pthread.h>
#include <ctype.h>

// For getuid() and geteuid()
#include <unistd.h>
#include <sys/types.h>

// For rate limiting packets

// Max length of packet to forward
#define SNAP_LEN 1540
// Not sure if interfaces need to be in promisc mode to capture traffic
#define PROMISC 1
// How many ms to wait between packet captures or something
#define TIMEOUT 10

// Size of interfaces array
#define MAX_IFACES 256
#define MAX_PORTS 2048

// Which character to use to separate out comments in config files
#define COMMENT_CHAR '#'

/**
 * Used to store a list of interfaces and their assocaited data
 **/
typedef struct
{
   pcap_t * pcap_int; // PCAP interface
   char * interface;  // String name of iface
   char pcap_errbuf[PCAP_ERRBUF_SIZE]; // Error buffer for PCAP to use
   bpf_u_int32 address; // IP address 
   bpf_u_int32 netmask; // Netmask 
   long int num_packets;
   long int num_dropped_packets;
} interface_data;

// List of interfaces. Later on, will make it input these as a file or arg
//char *iface_list[] = {"eth0", "eth1"};;
interface_data *iface_data;
char * iface_list[MAX_IFACES]; // This is kind of redundant; too late now.
int num_ifaces = 0;
int use_all_interfaces = 0;

// The list of ports from the input files get read in to this before 
// the filter string is made. 
char * port_list[MAX_PORTS];
int num_ports = 0;


// The PCAP/BPF filter string is stored in this. Generated in 
// generate_filter_string().
char * filter;

// To do: put in conditions for things to exito n
int do_exit = 0;

// Toggled with -d on command line. Displays debug info. 
int debug = 0;

// Does nothing at the moment. Will enable rewriting subnet info if relevant. 
int do_network_rewrite = 0; // Rewrite IP broadcast address for the new
                            // network interface
  
/**
  * Calculate checksum for IP packet.
  * Taken from http://stackoverflow.com/a/7010971
  * No idea what the license for it is; public domain? Hopefully!
  *
  * This isn't currently used in code. I was fiddling around with it earlier. 
  * Leaving here for future reference. 
  *
  * @param ptr Pointer to the frame contents
  * @param nbytes How many bytes long the frame is
  * @return The checksum.
  */
unsigned short in_cksum(unsigned short *ptr, int nbytes) {

    register long sum; /* assumes long == 32 bits */
    u_short oddbyte;
    register u_short answer; /* assumes u_short == 16 bits */
    /*
     * the algorithm is simple, using a 32-bit accumulator (sum),
     * we add sequential 16-bit words to it, and at the end, fold back
     * all the carry bits from the top 16 bits into the lower 16 bits.
     */
    sum = 0;
    while (nbytes > 1) {
        sum += *ptr++;
        nbytes -= 2;
    }

    /* mop up an odd byte, if necessary */
    if (nbytes == 1) {
        oddbyte = 0; /* make sure top half is zero */
        *((u_char *) &oddbyte) = *(u_char *) ptr; /* one byte only */
        sum += oddbyte;
    }

    /*
     * Add back carry outs from top 16 bits to low 16 bits.
     */
    sum = (sum >> 16) + (sum & 0xffff); /* add high-16 to low-16 */
    sum += (sum >> 16); /* add carry */
    answer = ~sum; /* ones-complement, then truncate to 16 bits */
    return (answer);
}


/**
 * Given a source interface name and a packet, flood that packet to every other
 * interface
 * 
 * @param args 
 * @param header The PCAP packet header (contains length of packet)
 * @param packet The packet to be flooded
 *
 **/
void flood_packet( u_char *source_iface, const struct pcap_pkthdr *header, const u_char *packet)
{
  int i;
  u_char * sendpacket;
  sendpacket = malloc(header->len);
  // This memcpy is only to let me experiment with modifying the packet. 
  // Not neccessary if not modifying packet, but easier to leave here. 
  memcpy(sendpacket, packet, header->len);
  

  if (timer_enabled)
  {
    // Offset of IP address in an IP header
    const bpf_u_int32 * srcipaddr =  (const bpf_u_int32 *) (packet+26);
    
    // Remember, if using this printf, cast type to unsigned char *
    //  printf("%hhu.%hhu.%hhu.%hhu\n", *(srcipaddr +0 ) , *(srcipaddr +1 ) , *(srcipaddr +2 ) , *(srcipaddr +3 ) );
   
    // NOTE TO SELF: move this to a function somewhere. 
    // And get rid of the magic numbers. 
    // Also put a check in to see if there's a malicious packet with "16" headers
    // 14 bytes is ethernet header
    // (packet+14) & 0x0F is the number of IP header lines in the packet (@4bytes ea)
    // 2 bytes is length of source port, which comes before dest
    unsigned short int * dstport =  (unsigned short int *)( packet +14 + (4 * ( *(packet+14) & 0x0F) ) + 2) ;
    printf("The port of that packet is  %hd \n",  ntohs(*dstport)) ;

    // Check if this packet hits the rate limiter
    if (timer_check_packet(srcipaddr, dstport) == SEND_PACKET)
    {
      if (debug)
        printf("Sent packet \n");
    }
    else
    {
      if (debug)
        printf("Drop packet\n");
      
      // TODO: increment packet_drop_count on iface data
      return;
    }
  }

  // Optionally rewrite the IP layer broadcast address to suit the new subnet
/*  if ( do_network_rewrite > 0)
  {
    // This resets the checksum to 0
    sendpacket[24] = 0x00;
    sendpacket[25] = 0x00;
    // Reset broadcast address to 255.255.255.255
    for (i = 30; i<34; i++)
      sendpacket[i] = 0xFF;
    // This isn't actually the packet checksum. One needs to create a pseudo 
    // header first; I'll do that later. 
    //printf("Packet checksum: %x\n", in_cksum((unsigned short *)sendpacket, header->len));
  }*/

  if (debug)
    printf("Packet length %d\n", header->len);

  for (i = 0; i < num_ifaces; i++)
  {
    if (strcmp(iface_list[i], (const char *)source_iface) != 0)
    {
      pcap_inject(iface_data[i].pcap_int, sendpacket, header->len);

    }
    else
    {
      iface_data[i].num_packets++;
    }
  }

  // we only malloc() this if we're modifying the frame
  //if (do_network_rewrite>0)   // malloc() moved outisde conditional above
  free(sendpacket);
}

/** 
 * Starts listening for incoming broadcasts, and sends them to be dealt with
 * Thread entry point
 *
 * @param args: The struct contains the name of the interface and the pcap_t pointer
 *
 **/
void *  start_listening(void * args)
{
  const interface_data * iface_data = (interface_data *)args;

  printf("Thread spawned\n");
  while (1)
  {
    if (do_exit) break;
    // The 2 is how many packets to process in each loop. Not sure what best 
    // value to have here is
    pcap_loop(iface_data->pcap_int, 2, flood_packet, (u_char *)iface_data->interface);
  }
  return NULL;
}

/** 
 * Initialises a PCAP interface and applies the filters we need
 *
 * @param interface: The name of the interface to use
 **/
pcap_t * init_pcap_int ( const char * interface, char * errbuf)
{
  pcap_t * ret;
  struct bpf_program  fp;
  if (debug)
    printf("Opening PCAP interface for %s\n", interface);

  // Create the pcap_t
  ret = pcap_open_live(interface, SNAP_LEN, PROMISC, TIMEOUT, errbuf);
  if (ret == NULL)
  {
    fprintf(stderr, "Error opening interface for listening");
    return NULL;
  }

  // Compile the filter for this interface
  if ( pcap_compile(ret, &fp, filter, 0, 0 ) == -1 ) {
      fprintf(stderr, "Error compiling filter");
      return NULL;
  }

  // Apply the filter
  if (pcap_setfilter(ret, &fp) == -1 ) {
    fprintf(stderr, "Error setting filter");
    return NULL;
  }

  // Only listen to input traffic
  if (pcap_setdirection(ret, PCAP_D_IN) == -1) {
    fprintf(stderr, "Error setting direction");
    return NULL;
  }
  return ret;
}

/** 
 * Read a file containing the list of settings. Reads from in, and puts each
 * line of non-comment non-whitespace input in to a string at dest[offest], 
 * where offset is incremented for each line. Whitespace around a setting is
 * trimmed. 
 *
 * Each line of the file should:
 * - Have any comments preceeded by a # on each line
 * - Be less than 256 characters long
 *
 * @param in pointer to the file to read
 * @return 0 on success, something else on failure. 
 * */
int parse_file(FILE * in, char * dest[], int * offset)
{
  char line[256];
  char * ptr;
  char * linestart;
//  char * lineend;
  char * comment;

  while (fgets( line, 256, in) != NULL)
  {
    // Stop the string at the comment if there is one
    if ( (comment = strchr(line, COMMENT_CHAR)) != NULL)
      *comment='\0';
    ptr=line;
    // Eat up white space at the start of the line
    while ( *ptr != '\0' && isspace(*ptr) )
      ptr++;
    linestart = ptr;
    // Eat up white space after the content
    while (*ptr != '\0' && isspace(*ptr) == 0)
      ptr++;
    *ptr = '\0';
    // Only put it on the dest array if there's something there!
    if (strlen(linestart) > 0)
    {
      dest[*offset] = malloc(strlen(linestart)+1);
      strcpy(dest[*offset], linestart);
      (*offset)++;
    }


  }

  return 0;

}


/** 
 * Takes the list of ports and turns it in to a PCAP compatible filter string
 * to select only configured port numbers.
 *
 * @param portlist An array containing strings containing port number(s). Each
 *                 item should either be a %d or a %d-%d, the latter indicating
 *                 a port range
 * @param numports The number of ports in the array
 * @return The string containing the PCAP filter
 * */
char * generate_filter_string(char * portlist[], int numports)
{
  // Giant string since we may might possibly have a fuck tonne of ports. 
  char *  ret = malloc(10000);
  char tmpstr[50] = "";
  char * end; // points to the end of the filter string

  // Filter needs to be only broadcast packets, and UDP
  strcpy(ret, "ether dst ff:ff:ff:ff:ff:ff and udp and (");

  // If we don't have at least one port, the string wont generate properly, 
  // and there will be no port restriction on the retransmitting, so we'll 
  // flood the network with DHCP and other lovely things. 
  if (numports < 1)
  {
    fprintf(stderr, "No ports specified. Exiting to avoid a network flood.");
    exit(-1);
  }
  end = ret + strlen(ret);

  for (int i = 0; i < numports; i++)
  {
    if (strchr(portlist[i], '-') != NULL)
    {
      // This is a port range
      sprintf(tmpstr, "portrange %s or ", portlist[i] );
    }
    else
    {
      // This is a single port
      sprintf(tmpstr, "port %s or ", portlist[i] );
    }

    // Append new port, and move end marker
    strcpy(end, tmpstr);
    end += strlen(tmpstr);
  }

  // Cut off the last "and ", and put a closing bracket on it, then EOF
  *(end-4) = ')';
  *(end-3) = '\0';

  return ret;
}

void printhelp()
{

    printf("\n\nService Discovery Helper\n(c) Chris 'SirSquidness' Holman, 2013\n \
Licensed under the MIT License\n\n \
Usage:\n \
  sudo ./sdh-proxy [-p ports-file -i interfaces-file [-d ]] [-h]\n \
  \n \
  Program must be run with PCAP capture+inject privileges (typically root)\n\
  \n \
  -p ports-file: List of ports are read from ports-file. Port ranges\n \
                 can be specified by using a hyphen, eg 10-50 \n \
  -i interfaces-file: List of interfaces are read from interfaces-file.\n \
  -a : Use all interfaces (ignores any interface files given) \n\
  -d : Turns on debug (doesn't do much yet\n \
  -h : Shows this help\n \
  \n\
  Multiple port and interface files can be specified.\n\
  \n\n");
}

/**
 * Sorry for the long main(). It just happened, okay?
 */
int main(int argc, char * argv[])
{
  // Stores a list of threads 
  pthread_t * threads;
  
  int i;

  if ( getuid() != 0 && geteuid() != 0)
  {
    fprintf(stderr, "Not running program as root. Crashes and segfaults may result.\n");
    fflush(stdout);
  }
  
  if (argc < 3)
  {
    printhelp();
    exit(-1);
  }
  // Read in arguments
  for (i = 1; i < argc; i++)
  {
    // -p, port list file
    if ( strcmp("-p", argv[i]) == 0)
    {
      if (++i < argc)
      {
        char * filename = argv[i];
        FILE * portfile;
        portfile = fopen(filename, "rt");
        if ( portfile == NULL || parse_file(portfile, port_list, &num_ports) != 0)
        {
          fprintf(stderr, "Error opening or parsing the ports list file, %s", filename);
          return -1;
        }
        fclose(portfile);
      }
      else
      {
        fprintf(stderr, "-p specified, but no filename");
        return -1;
      }

    }
    // -i, interface list file
    else if (strcmp("-i", argv[i]) == 0)
    {

      if (++i < argc)
      {
        char * filename = argv[i];
        FILE * ifacefile;
        ifacefile = fopen(filename, "rt");
        if ( ifacefile == NULL || parse_file(ifacefile, iface_list, &num_ifaces) != 0)
        {
          fprintf(stderr, "Error opening or parsing the interface list file, %s", filename);
          return -1;
        }
        fclose(ifacefile);
      }
      else
      {
        fprintf(stderr, "-i specified, but no filename");
        return -1;
      }
    }
    else if (strcmp("-t", argv[i]) == 0)
    {
      // -t takes a value
      if (++i < argc)
      {
        // value of -t is in argv[i] now
        pkt_timeout_s = 10;
        pkt_timeout_us = 0;
        printf("Haven't implemented this yet. ");
        return (-1);
      }
    }
    // -d, debug
    else if (strcmp("-d", argv[i]) == 0)
    {
      debug = 1;
    }
    // -a, all interfaces
    else if (strcmp("-a", argv[i]) == 0)
      use_all_interfaces = 1;
    // -h, help
    else if (strcmp("-h", argv[i]) == 0)
      printhelp();
    else
    {
      fprintf(stderr, "Unknown argument given. Exiting to avoid unexpected actions");
      printhelp();
      return -1;
    }

  }

  // If we're using all interfaces, get PCAP to tell us what ifaces are available
  if (use_all_interfaces)
  {
    pcap_if_t * firstdev;
    pcap_if_t * currentdev;
    char errbuf[PCAP_ERRBUF_SIZE] = "";
    
    
    // If someone specified an interface list AND -a, we'll just overwrite the iface list
    // Should probably throw an error, but whatever. free() anything created for the list
    for (int i = 0; i < num_ifaces; i++)
      free(iface_list[i]);
    // Then reset the count to 0.
    num_ifaces = 0;
    
    // Enumerate a list of all usable interfaces on the system
    if ( pcap_findalldevs( &firstdev, errbuf) == -1)
    {
      fprintf(stderr, "There was an error opening all devices. Maybe you aren't root.");
      fprintf(stderr, "%s\n", errbuf);
      return (-1);
    }
  
    for (currentdev = firstdev; currentdev; currentdev = currentdev->next)
    {
      // We don't want to listen on any USB interfaces, nor on the any 
      // interface. Listeningon the any interface is a bad idea, mmkay?
      // Add the iface to the iface_list if it's not those things. 
      if ( strstr(currentdev->name, "any") == NULL 
          && strstr(currentdev->name, "usb") == NULL)
      {
        printf("Detected and using interface: %s\n", currentdev->name);
        iface_list[num_ifaces] = malloc(strlen(currentdev->name));
        strcpy(iface_list[num_ifaces], currentdev->name);
        num_ifaces++;
      }
      
    }
    
    // free()s all of the stuff pcap_findalldevs created
    pcap_freealldevs(firstdev);
  }

  if (debug) 
  {
    printf("Ports being retransmitted:\n");
    for (int i = 0; i < num_ports; i++)
      printf("%s\n", port_list[i]);
    printf("Interfaces being listend and transmitted on:\n");
    for (int i = 0; i < num_ifaces; i++)
      printf("%s\n", iface_list[i]);
  }

  if (num_ports == 0 || num_ifaces == 0)
  {
    fprintf(stderr, "Either no ports or no interfaces specified. Nothing to do.");
    return (-1);
  }

  // PCAP filter 
  filter = generate_filter_string(port_list, num_ports);
  if (debug)
    printf("Using filter:%s\n", filter);


  iface_data = malloc( num_ifaces * sizeof(interface_data) );
  threads = malloc (num_ifaces * sizeof(pthread_t));
  

  // Create all of the interface listeners
  for (i = 0; i < num_ifaces; i++)
  {
    iface_data[i].interface =   iface_list[i];
    iface_data[i].pcap_int = init_pcap_int(iface_list[i], iface_data[i].pcap_errbuf );
    if (iface_data[i].pcap_int == NULL)
    {
      fprintf(stderr, "Couldn't create a listener for all interfaces. Exiting.");
      return -1;
    }
  }

  // Inits the thread lock in the timer 
  timer_init();

  // Once all pcap_ts are created, then spawn the processing threads
  for (i = 0; i< num_ifaces; i++)
  {
    pthread_create(&threads[i], NULL, start_listening, &iface_data[i]);
  }
 
  // Init the hash table purging thread
  // Don't care about joining it; when the program dies, it can die too. 
  if(timer_enabled)
    pthread_create(&threads[i++], NULL,timer_purge_old_entries_loop, NULL); 

  // Wait for all threads to finish before exiting
  for (i = 0; i< num_ifaces; i++)
  {
    pthread_join(threads[i], NULL);
  }

  return 0;
}
