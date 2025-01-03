/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include <stdlib.h>
#include <string.h>

/**
 * Connection state.
 * Stores per-connection information such as sequence numbers,
 * unacknowledged segments, retransmission counters, etc.
 */
struct ctcp_state {
    struct ctcp_state *next;
    struct ctcp_state **prev;

    conn_t *conn;
    linked_list_t *segments;   /* List of unacknowledged segments (Sliding Window) */

    uint32_t seqno;            /* Sequence number */
    uint32_t ackno;            /* Expected ACK number */
    uint32_t send_base;        /* Base of sending window (Sliding Window) */
    uint32_t recv_base;        /* Base of receiving window */

    uint32_t send_window;      /* Size of sending window (in bytes) */
    uint32_t recv_window;      /* Size of receiving window (in bytes) */

    int rt_timeout;            /* Retransmission timeout */
    int retransmit_count;      /* Retransmission count for Stop-and-Wait */

    long last_sent_time;       /* Last time a segment was sent */
};

static ctcp_state_t *state_list = NULL;


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
    if (!conn) return NULL;

    ctcp_state_t *state = calloc(1, sizeof(ctcp_state_t));
    state->conn = conn;

    state->send_window = cfg->send_window * MAX_SEG_DATA_SIZE;
    state->recv_window = cfg->recv_window * MAX_SEG_DATA_SIZE;
    state->rt_timeout = cfg->rt_timeout;

    state->seqno = 1;
    state->ackno = 0;
    state->send_base = 1;
    state->recv_base = 1;

    state->segments = ll_create();

    state->next = state_list;
    state->prev = &state_list;
    if (state_list) state_list->prev = &state->next;
    state_list = state;

    return state;
}


void ctcp_destroy(ctcp_state_t *state) {
    if (state->next) state->next->prev = state->prev;
    *state->prev = state->next;

    conn_remove(state->conn);
    ll_destroy(state->segments);
    free(state);
}

void ctcp_read(ctcp_state_t *state) {
    char buffer[MAX_SEG_DATA_SIZE];
    while (state->seqno < state->send_base + state->send_window) {
        int bytes_read = conn_input(state->conn, buffer, sizeof(buffer));
        if (bytes_read == 0) break;

        if (bytes_read == -1) { // EOF
            ctcp_segment_t fin_segment = {0};
            fin_segment.flags = htonl(TH_FIN);
            fin_segment.seqno = htonl(state->seqno);
            fin_segment.len = htonl(sizeof(ctcp_segment_t));
            conn_send(state->conn, &fin_segment, sizeof(ctcp_segment_t));
            state->seqno++;
            return;
        }

        ctcp_segment_t *segment = calloc(1, sizeof(ctcp_segment_t) + bytes_read);
        segment->seqno = htonl(state->seqno);
        segment->len = htonl(sizeof(ctcp_segment_t) + bytes_read);
        memcpy(segment->data, buffer, bytes_read);

        conn_send(state->conn, segment, sizeof(ctcp_segment_t) + bytes_read);
        ll_add(state->segments, segment);

        state->seqno += bytes_read;
        state->last_sent_time = current_time_ms();
    }
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
    uint32_t seqno = ntohl(segment->seqno);
    uint32_t flags = ntohl(segment->flags);

    if (flags & TH_ACK) {
        uint32_t ackno = ntohl(segment->ackno);
        if (ackno > state->send_base) {
            state->send_base = ackno;

            // Remove ACKed segments
            while (!ll_empty(state->segments)) {
                ctcp_segment_t *first = ll_front(state->segments);
                if (ntohl(first->seqno) < ackno) {
                    ll_remove(state->segments, first);
                    free(first);
                } else {
                    break;
                }
            }
        }
    }

    if (len > sizeof(ctcp_segment_t)) {
        size_t data_len = len - sizeof(ctcp_segment_t);
        conn_output(state->conn, segment->data, data_len);

        // Send ACK
        ctcp_segment_t ack_segment = {0};
        ack_segment.flags = htonl(TH_ACK);
        ack_segment.ackno = htonl(seqno + data_len);
        ack_segment.len = htonl(sizeof(ctcp_segment_t));
        conn_send(state->conn, &ack_segment, sizeof(ctcp_segment_t));
    }

    if (flags & TH_FIN) {
        conn_output(state->conn, NULL, 0); // EOF
        ctcp_destroy(state);
    }

    free(segment);
}

void ctcp_output(ctcp_state_t *state) {
    while (!ll_empty(state->segments)) {
        ctcp_segment_t *segment = ll_front(state->segments);
        uint32_t seqno = ntohl(segment->seqno);
        size_t data_len = ntohs(segment->len) - sizeof(ctcp_segment_t);

        if (seqno == state->recv_base) {
            conn_output(state->conn, segment->data, data_len);
            state->recv_base += data_len;

            ll_remove(state->segments, segment);
            free(segment);
        } else {
            break;
        }
    }
}


void ctcp_timer() {
    ctcp_state_t *state = state_list;

    while (state) {
        ll_node_t *node = state->segments->head;
        while (node) {
            ctcp_segment_t *segment = node->data;

            if (current_time_ms() - state->last_sent_time > state->rt_timeout) {
                conn_send(state->conn, segment, ntohs(segment->len));
                state->retransmit_count++;
                state->last_sent_time = current_time_ms();

                if (state->retransmit_count >= MAX_NUM_XMITS) {
                    ctcp_destroy(state);
                }
            }
            node = node->next;
        }
        state = state->next;
    }
}

void ctcp_connect_to_server(const char *hostname, int port) {
    struct sockaddr_in server_addr;
    struct hostent *server;

    // Resolve hostname to IP address
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr, "ERROR: No such host\n");
        exit(1);
    }

    // Set up server address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);

    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("ERROR: Could not open socket");
        exit(1);
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("ERROR: Connection failed");
        exit(1);
    }

    printf("Connected to %s:%d\n", hostname, port);

    // Send HTTP GET request
    char request[] = "GET / HTTP/1.1\r\nHost: google.com\r\nConnection: close\r\n\r\n";
    send(sockfd, request, strlen(request), 0);

    // Receive response
    char buffer[2048];
    int bytes_received;
    while ((bytes_received = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_received] = '\0';
        printf("%s", buffer);
    }

    close(sockfd);
}

/*main function*/
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <hostname> <port>\n", argv[0]);
        exit(1);
    }

    const char *hostname = argv[1];
    int port = atoi(argv[2]);

    // Kết nối đến web server
    ctcp_connect_to_server(hostname, port);

    return 0;
}
