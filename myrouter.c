#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <signal.h>
#include <time.h>

// Size of the buffer for packet payload
#define BUFFER_SIZE 65536

// Maximum number of routers supported, not including this one
// (Really only needs to be 5 for the purposes of the project)
#define DV_CAPACITY 16

#define MAX_LINE_LEN 80 // Max line size in topology file (for fgets)
#define MAX_BODY_LEN 81 // Max size of msg body of data packet
#define LOG_FILE_NAME_LEN 256
#define MAX_POSSIBLE_COST 64

enum packet_type {
    DATA_PACKET = 1,
    DV_PACKET = 2,
    KILLED_PACKET = 3,
    INITIAL_DV_PACKET = 4
};

struct dv_entry {
    uint16_t dest_port;
    uint16_t first_hop_port;
    uint32_t cost;
};
void ntoh_dv_entry(struct dv_entry *n, struct dv_entry *h) {
    h->dest_port = ntohs(n->dest_port);
    h->first_hop_port = ntohs(n->first_hop_port);
    h->cost = ntohl(n->cost);
}
void hton_dv_entry(struct dv_entry *h, struct dv_entry *n) {
    n->dest_port = htons(h->dest_port);
    n->first_hop_port = htons(h->first_hop_port);
    n->cost = htonl(h->cost);
}

// Linear search of an array of DV entries
struct dv_entry *
dv_find(struct dv_entry *dv, int dv_length, uint16_t dest_port) {
    int i;
    for (i=0; i<dv_length; i++) {
        if (dv[i].dest_port == dest_port) {
            return &(dv[i]);
        }
    }
    return NULL;
}

// Singly linked list of information about neighboring nodes
struct neighbor_list_node {
    uint16_t port;
    uint32_t cost;
    struct dv_entry *dv; // The neighbor node's DV (an array of DV entries)
    int dv_length; // Number of entries in the neighbor node's DV
    struct neighbor_list_node *next;
};

struct neighbor_list_node *
neighbor_list_find(struct neighbor_list_node *list_head, uint16_t port) {
    struct neighbor_list_node *node = list_head;
    for (; node!=NULL; node = node->next) {
        if (node->port == port) {
            return node;
        }
    }
    return NULL;
}

//-----------------------------------------------------------------------------
// Global variables
char my_label; // used find immediate neighbors in topology files
// Note: we assume node names are single char
uint16_t my_port;
struct dv_entry my_dv[DV_CAPACITY];
int my_dv_length = 0;
struct neighbor_list_node *my_neighbor_list_head = NULL;
int my_socket_fd; // Needs to be global for sig handler
FILE *log_file;
//-----------------------------------------------------------------------------

void send_message(int socket_fd, char *message, size_t message_length,
        uint16_t dest_port) {
    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    dest_addr.sin_port = htons(dest_port);
    if (sendto(socket_fd, message, message_length,
            0, (struct sockaddr *) &dest_addr, sizeof dest_addr) < 0) {
        perror("Local error trying to send packet");
    }
}

void print_my_dv() {
    fprintf(stdout, "Entries in my DV:\n");
    fprintf(log_file, "Entries in my DV:\n");

    int i;
    for (i = 0; i < my_dv_length; i++) {
        fprintf(stdout, "Dest port %u first hop port %u cost %u\n",
                my_dv[i].dest_port, my_dv[i].first_hop_port, my_dv[i].cost);
        fprintf(log_file, "Dest port %u first hop port %u cost %u\n",
                my_dv[i].dest_port, my_dv[i].first_hop_port, my_dv[i].cost);
    }
    fprintf(log_file, "\n");
    fflush(log_file);
}

void create_dv_message(char *buffer, enum packet_type type) {
    // See comment on message format
    buffer[0] = (char) type;
    struct dv_entry *dv = ((struct dv_entry *) buffer)+1;
    int i;
    for (i=0; i<my_dv_length; i++) {
        hton_dv_entry(&(my_dv[i]), &(dv[i]));
    }
}

void send_my_dv(int socket_fd, uint16_t dest_port) {
    printf("Sending DV to port %u\n", dest_port);
    char message[(DV_CAPACITY+1)*(sizeof(struct dv_entry))];
    create_dv_message(message, DV_PACKET);

    send_message(socket_fd, message,
            (my_dv_length+1)*(sizeof(struct dv_entry)), dest_port);
}

void broadcast_my_dv(int socket_fd, enum packet_type type) {
    printf("Sending DV broadcast\n");
    char message[(DV_CAPACITY+1)*(sizeof(struct dv_entry))];
    create_dv_message(message, type);

    struct neighbor_list_node *node = my_neighbor_list_head;
    for (; node!=NULL; node = node->next) {
        send_message(socket_fd, message,
                (my_dv_length+1)*(sizeof(struct dv_entry)), node->port);
    }
}

// Returns 1 if DV was changed.
// Returns 0 if not.
// Returns a negative number if an error occured.
int bellman_ford_decrease(uint16_t dest_port, uint16_t sender_port,
        uint32_t cost_thru_sender) {
    if (dest_port == my_port) {
        return 0;
    }
    struct dv_entry *e = dv_find(my_dv, my_dv_length, dest_port);
    if (e == NULL) {
        if (cost_thru_sender >= MAX_POSSIBLE_COST) {
            return 0;
        }
        if (my_dv_length >= DV_CAPACITY) {
            // Not necessarily the right thing to do
            printf("Warning: DV is full, new entry is discarded\n");
            return -1;
        } else {
            my_dv[my_dv_length].dest_port = dest_port;
            my_dv[my_dv_length].first_hop_port = sender_port;
            my_dv[my_dv_length].cost = cost_thru_sender;
            printf("DV update: New entry: Dest %u first hop %u cost %u\n",
                    my_dv[my_dv_length].dest_port,
                    my_dv[my_dv_length].first_hop_port,
                    my_dv[my_dv_length].cost);
            my_dv_length++;
            return 1;
        }
    } else if (cost_thru_sender >= MAX_POSSIBLE_COST) {
        // The target is now unreachable, so delete its entry from
        //  my_dv. We'll do this by moving the last entry into the
        //  deleted entry's slot.
        printf("DV update: Deletion: Dest %u no longer reachable\n",
                dest_port);
        *e = my_dv[my_dv_length-1];
        my_dv_length--;
        return 1;
    } else if (cost_thru_sender < e->cost) {
        printf("DV update: Entry for dest %u changed ", dest_port);
        printf("    from first hop %u cost %u ", e->first_hop_port, e->cost);
        e->first_hop_port = sender_port;
        e->cost = cost_thru_sender;
        printf("    to first hop %u cost %u ", e->first_hop_port, e->cost);
        return 1;
    } else {
        return 0;
    }
}

// DV message format:
// 1 byte indicating that this is a DV packet
// Padding to fill the length of a dv_entry
// 0 or more DV entries
//
// Returns the number of changes made to the DV, or a negative number if error
int handle_dv_packet(uint16_t sender_port, char *buffer,
        ssize_t bytes_received) {
    if (bytes_received % sizeof(struct dv_entry) != 0) {
        printf("Message not understood, length is not multiple of dv entry size\n");
        return -1;
    }
    printf("DV packet from port %u:\n", sender_port);

    struct neighbor_list_node *sender =
            neighbor_list_find(my_neighbor_list_head, sender_port);
    if (sender == NULL) {
        // Not necessarily the right thing to do
        printf("Warning: Sender is not a known neighbor; ignoring its message\n");
        return -1;
    }

    int received_dv_length = (bytes_received / sizeof(struct dv_entry)) - 1;
    if (received_dv_length > DV_CAPACITY) {
        printf("Received DV has %d entries, which exceeds the capacity of %d\n",
                received_dv_length, DV_CAPACITY);
        return -1;
    }

    struct dv_entry *raw_received_dv = ((struct dv_entry *) buffer) + 1;
    int i;
    for (i=0; i<received_dv_length; i++) {
        ntoh_dv_entry(&(raw_received_dv[i]), &(sender->dv[i]));
    }
    sender->dv_length = received_dv_length;

    for (i=0; i<sender->dv_length; i++) {
        printf("Entry: Dest port %u first hop port %u cost %u\n",
                sender->dv[i].dest_port, sender->dv[i].first_hop_port,
                sender->dv[i].cost);
    }

    int change_count = 0;
    
    // The Bellman-Ford part operates in two parts:
    // First, look for all entries in the current DV which have their first hop
    //  designated as the sender (unless the first hop is the sender itself).
    // If the DV received from the sender causes that cost to *increase*, then
    //  we have to look at the DVs from all the neighbors to see who now gives
    //  the lowest cost (or if the target is now unreachable altogether).
    i = 0;
    while(i<my_dv_length) {
        if (my_dv[i].first_hop_port == sender_port &&
                my_dv[i].dest_port != sender_port) {
            struct dv_entry *senders_entry =
                    dv_find(sender->dv, sender->dv_length, my_dv[i].dest_port);
            if (senders_entry==NULL ||
                    sender->cost + senders_entry->cost > my_dv[i].cost) {
                uint32_t min_cost = UINT32_MAX;
                uint16_t best_first_hop_port;
                int is_reachable = 0;
                struct neighbor_list_node *neighbor;
                for (neighbor = my_neighbor_list_head; neighbor != NULL;
                        neighbor = neighbor->next) {
                    struct dv_entry *neighbors_entry = dv_find(neighbor->dv,
                            neighbor->dv_length, my_dv[i].dest_port);
                    if (neighbors_entry!=NULL &&
                            neighbors_entry->cost + neighbor->cost < min_cost) {
                        min_cost = neighbors_entry->cost + neighbor->cost;
                        best_first_hop_port = neighbor->port;
                        is_reachable = 1;
                    }
                }
                if (is_reachable && min_cost < MAX_POSSIBLE_COST) {
                    my_dv[i].first_hop_port = best_first_hop_port;
                    my_dv[i].cost = min_cost;
                    change_count++;
                    i++;
                } else {
                    // The target is now unreachable, so delete its entry from
                    //  my_dv. We'll do this by moving the last entry into the
                    //  deleted entry's slot.
                    printf("DV update: Deletion: Dest %u no longer reachable\n",
                            my_dv[i].dest_port);
                    my_dv[i] = my_dv[my_dv_length-1];
                    my_dv_length--;
                    change_count++;
                    // i remains unchanged
                }
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    // Second, we do the standard Bellman-Ford: If the cost to go through the
    //  sender is now better than the old cost, then update the DV entry.
    for (i=0; i < sender->dv_length; i++) {
        uint16_t dest_port = sender->dv[i].dest_port;
        uint32_t cost_thru_sender = sender->cost + sender->dv[i].cost;
        if(bellman_ford_decrease(dest_port, sender_port,
                cost_thru_sender) > 0) {
            change_count++;
        }
    }
    // Finally, if my DV doesn't have an entry for the sender itself (because
    //  previously the sender was not alive), add an entry.
    if (bellman_ford_decrease(sender_port, sender_port, sender->cost) > 0) {
        change_count++;        
    }

    if (change_count > 0) {
        print_my_dv();
    } else {
        printf("DV did not change\n");
    }
    return change_count;
}

// handle SIGINT, SIGQUIT, SIGTERM by informing neighbors the router is killed
// Note: the SIGKILL signal (posix) can't be handled/caught
void handle_kill_signal(int sig) {

    // send dying message to all neighbors
    // message consists of KILLED_PACKET, padded to length of single dv_entry

    printf("Sending Killed broadcast\n");
    char message = KILLED_PACKET;

    struct neighbor_list_node *node = my_neighbor_list_head;
    for (; node!=NULL; node = node->next) {
        send_message(my_socket_fd, &message, 1, node->port);
    }

    signal(sig, SIG_DFL); // Restore default behavior
    raise(sig);
    return;
}

void handle_killed_packet(uint16_t sender_port) {
    // Note: doesn't matter what rest of message is, just that neighbor was killed
    printf("Killed_packet from port %u:\n", sender_port);

    struct neighbor_list_node *sender =
            neighbor_list_find(my_neighbor_list_head, sender_port);
    if (sender == NULL) {
        // Not necessarily the right thing to do
        printf("Warning: Sender is not a known neighbor; ignoring its message\n");
        return;
    }

    // Dead neighbor is now unreachable, so delete its entry from my_dv. We'll
    //  do this by moving the last entry into the deleted entry's slot.
    struct dv_entry *to_delete = dv_find(my_dv, my_dv_length, sender->port);
    if (to_delete == NULL) {
        printf("Warning: Sender not found in my_dv, may have already been removed\n");
        return;
    }
    printf("DV update: Deletion: Neighbor %u died\n", to_delete->dest_port);

    // Update rest of DV table (anything with dead neighbor as first hop is affected)
    // use dummy buffer to call BF alg in handle_dv_packet
    char buffer[sizeof(struct dv_entry)]; // buffer the size of dv entry
    handle_dv_packet(sender_port, buffer, sizeof(struct dv_entry));

    to_delete = dv_find(my_dv, my_dv_length, sender->port);
    if (to_delete != NULL) {
        printf("Neighbor %u didn't get deleted first time, deleting now\n", sender_port);
        *to_delete = my_dv[my_dv_length-1];
        my_dv_length--;
    }

    broadcast_my_dv(my_socket_fd, DV_PACKET);
    printf("Finished dv_table update and broadcast following Killed_packet from port %u:\n", sender_port);

    return;
}

void print_hexadecimal(char *bytes, int length) {
    int i;
    for (i=0; i<length; i++) {
        if (i!=0) {
            if (i%16 == 0)
                printf("\n");
            else if (i%4 == 0)
                printf(" ");
        }
        printf("%02X", bytes[i] & 0xFF);
    }
}

void handle_data_packet( uint16_t sender_port, char *buffer ){

    char bodybuf[81]; 
    strncpy(bodybuf, buffer+5, MAX_BODY_LEN); //message body
    //uint16_t dest_port = buffer[2] | uint16_t(buffer[3]) << 8;
    uint16_t dest_port = ntohs(*((uint16_t *) &(buffer[3])));
    //check if we are at at the destined router

    time_t ltime;
    ltime = time(NULL);

    fprintf(log_file, "Timestamp %s sourceID %c destID %c arrivalPort %u prevPort %u\n", 
        asctime(localtime(&ltime)) , buffer[1], buffer[2], my_port, sender_port);
    fflush(log_file);
    printf("Timestamp %s sourceID %c destID %c arrivalPort %u prevPort %u\n", 
        asctime(localtime(&ltime)) , buffer[1], buffer[2], my_port, sender_port);

    if ( dest_port != my_port ){
        struct dv_entry *dv = dv_find(my_dv, my_dv_length, dest_port);
        if (dv == NULL) {
            fprintf(stderr, "DV entry not found for destination port %u\n", dest_port);
            return;
        }

        uint16_t next_port = dv->first_hop_port;
        fprintf(log_file, "next port %u\n", next_port);
        fflush(log_file);

        printf("next port %u\n", next_port);
        size_t msg_sz = 5*sizeof(char) + MAX_BODY_LEN + 1;
        send_message(my_socket_fd, buffer, msg_sz, next_port);
    }
    else {
        fprintf(log_file, "%s\n", bodybuf);
        fflush(log_file); // force it to write
        printf("Received message!\n%s\n", bodybuf);
    }


}

// Send a UDP packet in Bash using
//      echo -n "Test" > /dev/udp/localhost/10001
// Send hexadecimal bytes in Bash using
//      echo 54657374 | xxd -r -p > /dev/udp/localhost/10001
// Or instead of "... > /dev/udp/localhost/10001", use
//      ... | nc -u -p 12345 -w0 localhost 10001
// to specify the sending port (here, 12345) and not be Bash-specific.
void server_loop(int socket_fd) {
    char buffer[BUFFER_SIZE];
    struct sockaddr_in remote_addr;
    socklen_t remote_addr_len = sizeof remote_addr;
    ssize_t bytes_received = recvfrom(socket_fd, buffer, BUFFER_SIZE, 0,
            (struct sockaddr *) &remote_addr, &remote_addr_len);
    if (bytes_received < 0) {
        perror("Error receiving data");
        return;
    }

    uint16_t sender_port = ntohs(remote_addr.sin_port);
    uint32_t sender_ip_addr = ntohl(remote_addr.sin_addr.s_addr);
    unsigned char ip_bytes[4];
    ip_bytes[0] = sender_ip_addr & 0xFF;
    ip_bytes[1] = (sender_ip_addr>>8) & 0xFF;
    ip_bytes[2] = (sender_ip_addr>>16) & 0xFF;
    ip_bytes[3] = (sender_ip_addr>>24) & 0xFF;

    printf("Received %d bytes ", (int) bytes_received);
    printf("from IP address %u.%u.%u.%u ",
            ip_bytes[3], ip_bytes[2], ip_bytes[1], ip_bytes[0]);
    printf("port %u:\n", sender_port);
    // printf("ASCII: %.*s\n", (int) bytes_received, buffer);
    printf("Hexadecimal:\n");
    print_hexadecimal(buffer, bytes_received);
    printf("\n");

    if (bytes_received == 0) {
        printf("Message not understood, 0 bytes received\n");
        return;
    }

    switch (buffer[0]) {
        case DATA_PACKET:
            printf("Data packet received\n");
            handle_data_packet(sender_port, buffer);
        break;
        case DV_PACKET:
            if (handle_dv_packet(sender_port, buffer, bytes_received) > 0) {
                broadcast_my_dv(socket_fd, DV_PACKET);
            }
        break;
        case KILLED_PACKET:
            handle_killed_packet(sender_port);
        break;
        case INITIAL_DV_PACKET:
            if (handle_dv_packet(sender_port, buffer, bytes_received) > 0) {
                broadcast_my_dv(socket_fd, DV_PACKET);
            } else {
                send_my_dv(socket_fd, sender_port);
            }
        break;
        default:
            printf("Message not understood, packet type not recognized\n");
    }
    printf("\n");
}


struct neighbor_list_node *
new_neighbor_list_node(uint16_t port, uint16_t cost,
        struct neighbor_list_node *next) {
    struct neighbor_list_node *n = malloc(sizeof(struct neighbor_list_node));
    if (n==NULL) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }
    n->port = port;
    n->cost = cost;
    n->dv = malloc(DV_CAPACITY * sizeof(struct dv_entry));
    if (n->dv == NULL) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
    }
    n->dv_length = 0;
    n->next = next;
    return n;
}

void find_label(const char *file_name) {
    // find first edge where dest port matches this router's port
    // the dest label is then the label corresponding to this port

    FILE* f = fopen(file_name, "rt");
    char line[MAX_LINE_LEN];

    while (fgets(line, MAX_LINE_LEN, f) != NULL) {
        char src;
        char dest;
        uint16_t port;
        uint16_t cost;

        if (sscanf(line, "%c,%c,%" SCNd16 ",%" SCNd16 "", &src, &dest, &port, &cost) < 4){
            fprintf(stderr, "Error: cannot read network topology file");
            exit(1);
        }

        if (port == my_port){
            my_label = dest;
            fclose(f);
            return;
        }
    }
    // If port not found while scanning, error
    fclose(f);
    fprintf(stderr, "Error: Port number not in network topology file\n");
    exit(1);
}

// The router finds its immediate neighbors from file
// Initialize neighbors from tuples of 
//      <source router, destination router, destination UDP port, link cost>
// (seems to be destination port, even though spec says source port?)
void initialize_neighbors(const char *file_name) {

    struct neighbor_list_node *next = NULL; // tail node added first, has NULL next
    struct neighbor_list_node* current = NULL;

    FILE* f = fopen(file_name, "rt");
    char line[MAX_LINE_LEN];
    while(fgets(line, MAX_LINE_LEN, f) != NULL){
        char src;
        char dest;
        uint16_t port;
        uint16_t cost;

        if (sscanf(line, "%c,%c,%" SCNd16 ",%" SCNd16 "", &src, &dest, &port, &cost) < 4){
            fprintf(stderr, "Error: cannot read network topology file");
            exit(1);

        }

        if (src == my_label) {
            current = new_neighbor_list_node(port, cost, next);
            next = current;
        }
    }

    fclose(f);
    my_neighbor_list_head = current;
    return;
}

// prompts user for message body, then sends through src node with ultimate goal dest
int generate_traffic(char src_label, char dest_label, const char *topology_file_name){

    char bodybuf[81];
    bodybuf[80] = '\0';
    uint16_t src_port; // corresponding ports to given labels
    uint16_t dest_port;
    int src_port_found = 0;
    int dest_port_found = 0;


    printf("What message would you like to send from %c to %c? (up to 80 char)\n", 
        src_label, dest_label);

    // allow spaces until newline, discard newline
    if (scanf("%81[^\n]%*c", bodybuf) != 1) { 
        fprintf(stderr, "Error: Could not read message for traffic generation\n");
        exit(1);
    }

    FILE* f = fopen(topology_file_name, "rt");
    char line[MAX_LINE_LEN];
    while(fgets(line, MAX_LINE_LEN, f) != NULL){
        char src;
        char dest;
        uint16_t port; // dest port
        uint16_t cost;

        if (sscanf(line, "%c,%c,%" SCNd16 ",%" SCNd16 "", &src, &dest, &port, &cost) < 4){
            fprintf(stderr, "Error: cannot read network topology file\n");
            exit(1);
        }

        if (dest == src_label) {
            src_port = port;
            src_port_found = 1;
        }

        if (dest == dest_label) {
            dest_port = port;
            dest_port_found = 1;
        }
    }


    if (!(src_port_found && dest_port_found)) {
        fprintf(stderr, "Error: Cannot find both corresponding ports\n");
        exit(1);
    }


    // create socket
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("Error creating socket");
        exit(1);
    }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(my_port);
    if (bind(socket_fd,
            (struct sockaddr *) &server_addr,
            sizeof server_addr) < 0) {
        perror("Error binding socket");
        exit(1);
    }

    my_socket_fd = socket_fd; // set global too


    // send message to src port consisting of 
    //      DATA flag
    //      ultimate destination label
    //      ultimate destination port byte
    //      ultimate destination port byte
    //      message body

    // sloppy array stuff
    size_t msg_sz = 5*sizeof(char) + MAX_BODY_LEN + 1;
    char message[msg_sz];

    //char lo = dest_port & 0xFF;
    //char hi = dest_port >> 8;
    message[0] = (char) DATA_PACKET;
    message[1] = src_label;
    message[2] = dest_label;
    message[3] = htons(dest_port) & 0xFF;
    message[4] = htons(dest_port) >> 8;
    strncpy(message + 5, bodybuf, MAX_BODY_LEN);

    
    printf("Injecting data into network\n");
    send_message(socket_fd, message, msg_sz, src_port); 


    // write output

    char log_file_name[LOG_FILE_NAME_LEN];
    strcpy(log_file_name, "routing-output_.txt");
    log_file_name[14] = my_label; // will be 'H'
    log_file = fopen(log_file_name, "w");
    if (log_file == NULL) {
        fprintf(stderr, "Error: Failed to open log file %s\n", log_file_name);
        exit(1);
    }
    fprintf(log_file, "This is traffic generator %c on port %u\n", my_label, my_port);
    fprintf(log_file, "Sending a data packet to router %c on port %u\n", src_label, src_port);
    fprintf(log_file, "With ultimate destination being router %c on port %u\n", dest_label, dest_port);
    fprintf(log_file, "The message payload is as follows:\n");
    fprintf(log_file, "%s\n", bodybuf);


    return 0;
}



int str_to_uint16(const char *str, uint16_t *result) {
    char *end;
    errno = 0;
    long int value = strtol(str, &end, 10);
    if (errno == ERANGE || value > UINT16_MAX || value < 0
            || end == str || *end != '\0')
        return -1;
    *result = (uint16_t) value;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Error: No port number provided\n");
        exit(1);
    }


    char *port_no_str = argv[1];
    if (str_to_uint16(port_no_str, &my_port) < 0) {
        fprintf(stderr, "Error: Invalid port number %s\n", port_no_str);
        exit(1);
    }

    // if using this router as a traffic generator from initial point to dest
    // ex/       ./myrouter 10006 A D
    if (argc == 4) {
        // cannot use ports _between_ 10000 and 10005 bc they are reserved for network
        if (my_port >= 10000 && my_port <= 10005) {
            fprintf(stderr, "Error: Port number %s is reserved for in-network routers\n", port_no_str);
            exit(1);
        }

        my_label = 'H'; // traffic generator gets label H, not part of network
        // will prompt user for message and send to first specified node
        generate_traffic(argv[2][0], argv[3][0], "sample_topology.txt"); 

        return 0; // quit after injecting message
    }


    // TODO give user option to specify file
    find_label("sample_topology.txt"); // Find this node's own name
    initialize_neighbors("sample_topology.txt");

    char log_file_name[LOG_FILE_NAME_LEN];
    strcpy(log_file_name, "routing-output_.txt");
    log_file_name[14] = my_label;
    log_file = fopen(log_file_name, "w");
    if (log_file == NULL) {
        fprintf(stderr, "Error: Failed to open log file %s\n", log_file_name);
        exit(1);
    }
    fprintf(log_file, "This is router %c on port %u\n", my_label, my_port);

    struct neighbor_list_node *node = my_neighbor_list_head;
    fprintf(stdout, "My neighbors are:\n");
    for (; node!=NULL; node = node->next) {
        fprintf(stdout, "Port %u Cost %u\n", node->port, node->cost);
    }

    fprintf(stdout, "My label is %c\n\n", my_label);

    // AF_INET ---> IPv4
    // SOCK_DGRAM ---> UDP
    int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) {
        perror("Error creating socket");
        exit(1);
    }
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(my_port);
    if (bind(socket_fd,
            (struct sockaddr *) &server_addr,
            sizeof server_addr) < 0) {
        perror("Error binding socket");
        exit(1);
    }

    my_socket_fd = socket_fd; // set global too

    print_my_dv();
    broadcast_my_dv(socket_fd, INITIAL_DV_PACKET);
    printf("\n");

    // After this point (initial contact w/ neighbors), should let neighbors
    //  know if killed
    signal(SIGINT, handle_kill_signal);
    signal(SIGTERM, handle_kill_signal);
    signal(SIGQUIT, handle_kill_signal);

    while (1) {
        server_loop(socket_fd);
    }
    return 0;
}
