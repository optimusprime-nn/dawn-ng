#include <libubox/usock.h>
#include <arpa/inet.h>
#include <inttypes.h>

#include "memory_utils.h"
#include "msghandler.h"
#include "crypto.h"
#include "datastorage.h"
#include "tcpsocket.h"
#include "dawn_auth.h"
#include "dawn_stats.h"

#define STR_EVAL(x) #x
#define STR_QUOTE(x) STR_EVAL(x)

#define HEADER_SIZE sizeof(uint32_t)
// Upper bound on a single framed message (header included). Peers are trusted
// to be other DAWN instances, but the length prefix arrives off the wire and
// must never be used unchecked for allocation or buffer arithmetic.
#define MAX_TCP_MSG_LEN (4U * 1024U * 1024U)
#define PING_STR "ping"
#define PONG_STR "pong"
#define PING_SIZE (strlen(PING_STR)+1)
#define PONG_SIZE (strlen(PONG_STR)+1)

LIST_HEAD(tcp_sock_list);
LIST_HEAD(cli_list);

struct network_con_s *tcp_list_contains_address(struct sockaddr_in entry);

static struct uloop_fd server;
static struct client *next_client = NULL; // TODO: Why here? Only used in sever_cb()

enum socket_read_status {
    READ_STATUS_READY,
    READ_STATUS_COMMENCED,
    READ_STATUS_COMPLETE
};

struct client {
    struct list_head list;
    struct sockaddr_in sin;

    struct ustream_fd s;
    int ctr;
    int counter;
    char *str; // message buffer
    enum socket_read_status state; // messge read state
    uint32_t final_len; // full message length
    uint32_t curr_len; // bytes read so far
    time_t time_alive;
};


static void client_close(struct ustream *s) {
    dawnlog_debug_func("Entering...");

    struct client *cl = container_of(s, struct client, s.stream);

    dawnlog_warning("Connection closed\n");
    if (cl->str) {
        // A partially received message would otherwise leak here
        dawn_free(cl->str);
        cl->str = NULL;
    }
    ustream_free(s);
    dawn_unregmem(s);
    close(cl->s.fd.fd);
    list_del(&cl->list);
    dawn_free(cl);
    cl = NULL;
}

static void client_notify_write(struct ustream *s, int bytes) {
    return;
}


// FIXME: This void function tries to return a value sometimes...
static void client_notify_state(struct ustream *s) {
    dawnlog_debug_func("Entering...");

    struct client *cl = container_of(s, struct client, s.stream);
    if (!s->write_error && !s->eof)
        return;

    dawnlog_error("Closing client-connection, pending: %d, total: %d\n", s->w.data_bytes, cl->ctr);
    client_close(s);
}

// Tear down an outgoing (client-to-server) connection, whatever state it is in.
// Until connect_cb() runs the socket is watched via con->fd; afterwards it is
// owned by the ustream. Either way it must be removed from uloop before the
// memory is freed, or the event loop is left holding a dangling pointer.
static void tcp_con_close(struct network_con_s *con) {
    dawnlog_debug_func("Entering...");

    if (con->connected) {
        ustream_free(&con->stream.stream);
        dawn_unregmem(&con->stream.stream);
    } else {
        uloop_fd_delete(&con->fd);
    }

    close(con->fd.fd);
    list_del(&con->list);
    dawn_free(con);
}

static void client_to_server_state(struct ustream *s) {
    // This stream belongs to a network_con_s, not a struct client
    struct network_con_s *con = container_of(s, struct network_con_s, stream.stream);

    dawnlog_debug_func("Entering...");

    if (!s->write_error && !s->eof)
        return;

    dawnlog_error("Closing connection to %s, pending: %d\n", inet_ntoa(con->sock_addr.sin_addr), s->w.data_bytes);
    tcp_con_close(con);
}

// A peer without (or with the wrong) auth_key retries every update_tcp_con
// seconds, so log rejections at most once a minute.
static void auth_failure_log(const struct sockaddr_in *from, const char *reason) {
    static time_t last_log = 0;
    static unsigned suppressed = 0;
    time_t now = time(0);

    if (now - last_log < 60 && now >= last_log) {
        suppressed++;
        return;
    }

    dawnlog_warning("tcp_socket: dropping message from %s: %s%s\n", inet_ntoa(from->sin_addr),
            reason ? reason : "authentication failed",
            suppressed ? " (similar messages suppressed)" : "");
    last_log = now;
    suppressed = 0;
}

static void client_read_cb(struct ustream *s, int bytes) {
    struct client *cl = container_of(s, struct client, s.stream);

    dawnlog_debug_func("Entering...");

    while(1) {
        if (cl->state == READ_STATUS_READY)
        {
            dawnlog_debug("tcp_socket: commencing message...\n");
            cl->str = dawn_malloc(HEADER_SIZE);
            if (!cl->str) {
                dawnlog_error("not enough memory (" STR_QUOTE(__LINE__) ")\n");
                break;
            }

            uint32_t avail_len = ustream_pending_data(s, false);

            if (avail_len < HEADER_SIZE){//ensure recv sizeof(uint32_t)
                dawnlog_debug("not complete msg, len:%d\n", avail_len);
                dawn_free(cl->str);
                cl->str = NULL;
                break;
            }

            if (ustream_read(s, cl->str, HEADER_SIZE) != HEADER_SIZE) // read msg length bytes
            {
                dawnlog_error("msg length read failed\n");
                dawn_free(cl->str);
                cl->str = NULL;
                break;
            }

            cl->curr_len += HEADER_SIZE;
            uint32_t net_len;
            memcpy(&net_len, cl->str, sizeof(net_len)); // avoid unaligned/aliasing read
            cl->final_len = ntohl(net_len);

            // The length covers the header itself, so anything <= HEADER_SIZE is
            // malformed: it would make (final_len - curr_len) underflow below and
            // turn into a heap overflow. Oversized lengths are a memory DoS.
            // In either case the stream is desynchronised, so drop the peer.
            if (cl->final_len <= HEADER_SIZE || cl->final_len > MAX_TCP_MSG_LEN) {
                dawnlog_error("tcp_socket: invalid message length %" PRIu32 " from %s, closing\n",
                        cl->final_len, inet_ntoa(cl->sin.sin_addr));
                dawn_free(cl->str);
                cl->str = NULL;
                cl->curr_len = 0;
                cl->final_len = 0;
                s->eof = true;
                ustream_state_change(s); // deferred -> client_notify_state -> client_close
                return;
            }

            // On failure, dawn_realloc returns a null pointer. The original pointer str
            // remains valid and may need to be deallocated.
            // +1 so the payload can be NUL-terminated before it is parsed as a string.
            char *str_tmp = dawn_realloc(cl->str, cl->final_len + 1);
            if (!str_tmp) {
                dawnlog_error("not enough memory (%" PRIu32 " @ " STR_QUOTE(__LINE__) ")\n", cl->final_len);
                dawn_free(cl->str);
                cl->str = NULL;
                break;
            }

            cl->str = str_tmp;
            str_tmp = NULL; // Aboutt o go out of scope, but just in case it gets moved around...
            cl->state = READ_STATUS_COMMENCED;
        }

        if (cl->state == READ_STATUS_COMMENCED)
        {
            dawnlog_debug("tcp_socket: reading message...\n");
            uint32_t read_len = ustream_pending_data(s, false);

            if (read_len == 0)
                break;

            if (read_len > (cl->final_len - cl->curr_len))
                    read_len = cl->final_len - cl->curr_len;

            dawnlog_debug("tcp_socket: reading %" PRIu32 " bytes to add to %" PRIu32 " of %" PRIu32 "...\n",
                    read_len, cl->curr_len, cl->final_len);

            uint32_t this_read = ustream_read(s, cl->str + cl->curr_len, read_len);
            cl->curr_len += this_read;
            dawnlog_debug("tcp_socket: ...and we're back, now have %" PRIu32 " bytes\n", cl->curr_len);
            if (cl->curr_len == cl->final_len){//ensure recv final_len bytes.
                // Full message now received. Senders include the NUL, but that is
                // not something we can rely on from the network.
                cl->str[cl->final_len] = '\0';
                cl->state = READ_STATUS_COMPLETE;
                dawnlog_debug("tcp_socket: message completed\n");
            }
        }

        if (cl->state == READ_STATUS_COMPLETE)
        {
            dawnlog_debug("tcp_socket: processing message...\n");

            /* received pong */
            if (cl->final_len == HEADER_SIZE + PONG_SIZE && memcmp(cl->str + HEADER_SIZE, PONG_STR, PONG_SIZE) == 0) {
                dawnlog_info("Server: received pong from %s:%u, now=%d\n", inet_ntoa(cl->sin.sin_addr), ntohs(cl->sin.sin_port), (int)time(0));
                goto process_done;
            }

            char *body = cl->str + HEADER_SIZE;
            size_t body_len = cl->final_len - HEADER_SIZE;
            bool authenticated = false;

            dawn_stats.peer_msgs_received++;

            if (dawn_auth_enabled()) {
                const char *reason = NULL;
                size_t payload_len = 0;
                bool encrypted = body_len >= DAWN_AUTH_MAGIC_LEN && memcmp(body, DAWN_ENC_MAGIC, DAWN_AUTH_MAGIC_LEN) == 0;
                char *payload = dawn_auth_unwrap(body, body_len, time(0), &payload_len, &reason);

                if (!payload) {
                    dawn_stats.peer_msgs_rejected_auth++;
                    auth_failure_log(&cl->sin, reason);
                    goto process_done;
                }

                body = payload;
                if (encrypted)
                    dawn_stats.peer_msgs_encrypted++;
                body_len = payload_len;
                authenticated = true;
            }

            if (network_config.use_symm_enc) {
                char *dec = gcrypt_decrypt_msg(body, body_len);
                if (!dec) {
                    // Bad ciphertext length, cipher error or OOM. Drop just this
                    // message; the framing is intact so the stream can continue.
                    dawnlog_error("tcp_socket: failed to decrypt message from %s, dropping\n",
                            inet_ntoa(cl->sin.sin_addr));
                    goto process_done;
                }
                handle_network_msg(dec, authenticated);
                dawn_free(dec);
                dec = NULL;
            } else {
                // NUL-terminated: cl->str[final_len] was set when the frame completed
                handle_network_msg(body, authenticated);
            }

process_done:
            cl->state = READ_STATUS_READY;
            cl->curr_len = 0;
            cl->final_len = 0;
            dawn_free(cl->str);
            cl->str = NULL;
            cl->time_alive = time(0);
        }
    }

    dawnlog_debug("tcp_socket: leaving\n");
    return;
}

static void server_cb(struct uloop_fd *fd, unsigned int events) {
    struct client *cl; //MUSTDO: check free() of this
    unsigned int sl = sizeof(struct sockaddr_in);
    int sfd;

    dawnlog_debug_func("Entering...");

    if (!next_client)
        next_client = dawn_calloc(1, sizeof(*next_client));

    if (!next_client) {
        dawnlog_error("server_cb: not enough memory\n");
        return;
    }

    cl = next_client;

    sfd = accept(server.fd, (struct sockaddr *) &cl->sin, &sl);
    if (sfd < 0) {
        dawnlog_error("Accept failed\n");
        return;
    }

    cl->s.stream.string_data = 1;
    cl->s.stream.notify_read = client_read_cb;
    cl->s.stream.notify_state = client_notify_state;
    cl->s.stream.notify_write = client_notify_write;
    cl->time_alive = time(0);
    list_add(&cl->list, &cli_list);
    ustream_fd_init(&cl->s, sfd);
    dawn_regmem(&cl->s);
    next_client = NULL;  // TODO: Why is this here?  To avoid resetting if above return happens?
    dawnlog_info("New connection\n");
}

int run_server(char *ipv4, int port) {
    dawnlog_debug("Adding socket!\n");
    char port_str[12];
    sprintf(port_str, "%d", port); // TODO: Manage buffer length

    server.cb = server_cb;
    server.fd = usock(USOCK_TCP | USOCK_SERVER | USOCK_IPV4ONLY | USOCK_NUMERIC, ipv4, port_str);
    if (server.fd < 0) {
        dawnlog_perror("usock");
        return 1;
    }

    uloop_fd_add(&server, ULOOP_READ);

    return 0;
}

// The only thing a server ever sends down our outgoing connection is a ping.
static void client_ping_read_cb(struct ustream *s, int bytes) {
    struct network_con_s *con = container_of(s, struct network_con_s, stream.stream);
    const uint32_t ping_len = HEADER_SIZE + PING_SIZE;
    char buf[HEADER_SIZE + PING_SIZE + 1];

    dawnlog_debug_func("Entering...");

    // TCP may split or coalesce writes: only consume whole pings, and leave any
    // partial one in the stream for next time (previously it was discarded,
    // desynchronising every later read on the connection).
    while (ustream_pending_data(s, false) >= ping_len) {
        int len = ustream_read(s, buf, ping_len);
        uint32_t net_len;

        if (len != (int)ping_len)
            break;

        memcpy(&net_len, buf, sizeof(net_len));
        if (ntohl(net_len) != ping_len || memcmp(buf + HEADER_SIZE, PING_STR, PING_SIZE) != 0) {
            buf[len] = '\0';
            dawnlog_error("Unexpected %d bytes from %s, closing\n", len, inet_ntoa(con->sock_addr.sin_addr));
            // Can't free the stream from inside its own read callback: libubox
            // still uses it on return. Defer to client_to_server_state().
            s->eof = true;
            ustream_state_change(s);
            return;
        }

        /* client received ping, send pong back to server */
        uint32_t final_len = HEADER_SIZE + PONG_SIZE;
        char final_str[HEADER_SIZE + PONG_SIZE];
        uint32_t msg_header = htonl(final_len);

        dawnlog_info("Client: received ping from %s:%u, now=%d\n", inet_ntoa(con->sock_addr.sin_addr), ntohs(con->sock_addr.sin_port), (int)time(0));
        con->time_alive = time(0);

        memcpy(final_str, &msg_header, HEADER_SIZE);
        memcpy(final_str + HEADER_SIZE, PONG_STR, PONG_SIZE);
        if (ustream_write(&con->stream.stream, final_str, final_len, 0) <= 0) {
            dawnlog_error("Ustream error(" STR_QUOTE(__LINE__) ")!\n");
            // On write_error libubox has already scheduled a state change, which
            // closes the connection via client_to_server_state() once we return.
            if (con->stream.stream.write_error)
                return;
        }
    }
}

static void connect_cb(struct uloop_fd *f, unsigned int events) {

    struct network_con_s *entry = container_of(f, struct network_con_s, fd);

    dawnlog_debug_func("Entering...");

    if (f->eof || f->error) {
        dawnlog_error("Connection failed (%s)\n", f->eof ? "EOF" : "ERROR");
        tcp_con_close(entry); // not yet connected, so also unregisters from uloop
        return;
    }

    dawnlog_debug("Connection established\n");
    uloop_fd_delete(&entry->fd);

    entry->stream.stream.notify_read = client_ping_read_cb;
    entry->stream.stream.notify_state = client_to_server_state;

    ustream_fd_init(&entry->stream, entry->fd.fd);
    dawn_regmem(&entry->stream);

    entry->connected = 1;
}

int add_tcp_connection(char *ipv4, int port) {
    struct sockaddr_in serv_addr;

    dawnlog_debug_func("Entering...");

    char port_str[12];
    sprintf(port_str, "%d", port); // TODO: Manage buffer length

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(ipv4);
    serv_addr.sin_port = htons(port);

    struct network_con_s *tmp = tcp_list_contains_address(serv_addr);
    if (tmp != NULL) {
        if(tmp->connected == true)
        {
            return 0;
        } else{
            // Delete already existing (still connecting) entry
            tcp_con_close(tmp);
            tmp = NULL;
        }
    }

    struct network_con_s *tcp_entry = dawn_calloc(1, sizeof(struct network_con_s));
    if (!tcp_entry) {
        dawnlog_error("add_tcp_connection: not enough memory\n");
        return -1;
    }
    tcp_entry->fd.fd = usock(USOCK_TCP | USOCK_NONBLOCK, ipv4, port_str);
    tcp_entry->sock_addr = serv_addr;

    if (tcp_entry->fd.fd < 0) {
        dawn_free(tcp_entry);
        tcp_entry = NULL;
        return -1;
    }
    tcp_entry->fd.cb = connect_cb;
    uloop_fd_add(&tcp_entry->fd, ULOOP_WRITE | ULOOP_EDGE_TRIGGER);

    dawnlog_warning("New TCP connection to %s:%d\n", ipv4, port);
    tcp_entry->time_alive = time(0);
    list_add(&tcp_entry->list, &tcp_sock_list);

    return 0;
}

void send_tcp(char *msg) {
    dawnlog_debug_func("Entering...");

    if (dawnlog_showing(DAWNLOG_DEBUG))
        print_tcp_array();

    // Build the payload: the JSON (with its NUL), optionally encrypted, then
    // optionally authenticated. Finally prefix the length and send to all peers.
    char *payload = msg;
    size_t payload_len = strlen(msg) + 1;
    char *enc = NULL, *auth = NULL, *final_str = NULL;

    if (network_config.use_symm_enc) {
        int length_enc = 0;
        enc = gcrypt_encrypt_msg(msg, payload_len, &length_enc);
        if (!enc) {
            dawnlog_error("Ustream error: not enough memory (" STR_QUOTE(__LINE__) ")\n");
            return;
        }
        payload = enc;
        payload_len = (size_t)length_enc;
    }

    if (dawn_auth_enabled()) {
        size_t auth_len = 0;
        auth = dawn_auth_wrap(payload, payload_len, time(0), &auth_len);
        if (!auth) {
            dawnlog_error("send_tcp: failed to authenticate message, not sending\n");
            goto out;
        }
        payload = auth;
        payload_len = auth_len;
    }

    if (payload_len > MAX_TCP_MSG_LEN - HEADER_SIZE) {
        dawnlog_error("send_tcp: message too large (%zu bytes), not sending\n", payload_len);
        goto out;
    }

    uint32_t final_len = (uint32_t)(payload_len + HEADER_SIZE);
    final_str = dawn_malloc(final_len);
    if (!final_str) {
        dawnlog_error("Ustream error: not enough memory (" STR_QUOTE(__LINE__) ")\n");
        goto out;
    }

    uint32_t msg_header = htonl(final_len);
    memcpy(final_str, &msg_header, HEADER_SIZE);
    memcpy(final_str + HEADER_SIZE, payload, payload_len);

    struct network_con_s *con, *tmp;
    list_for_each_entry_safe(con, tmp, &tcp_sock_list, list)
    {
        if (con->connected) {
            int len_ustream = ustream_write(&con->stream.stream, final_str, final_len, 0);
            dawnlog_debug("Ustream send: %d\n", len_ustream);
            if (len_ustream <= 0) {
                dawnlog_error("Ustream error(" STR_QUOTE(__LINE__) ")!\n");
                if (con->stream.stream.write_error) {
                    tcp_con_close(con);
                    con = NULL;
                }
            }
        }
    }

out:
    if (final_str)
        dawn_free(final_str);
    if (auth)
        dawn_free(auth);
    if (enc)
        dawn_free(enc);
}

void server_to_clients_ping(void)
{
    struct client *cl, *tmp;
    const char *msg = PING_STR;
    size_t msglen = PING_SIZE;
    uint32_t final_len = msglen + HEADER_SIZE;
    char final_str[HEADER_SIZE + PING_SIZE];
    uint32_t *msg_header = (uint32_t *)final_str;
    *msg_header = htonl(final_len);
    memcpy(final_str + HEADER_SIZE, msg, msglen);

    list_for_each_entry_safe(cl, tmp, &cli_list, list)
    {
        int len_ustream = ustream_write(&cl->s.stream, final_str, final_len, 0);
        if (len_ustream <= 0) {
            dawnlog_error("Ustream error(" STR_QUOTE(__LINE__) ")!\n");
            if (cl->s.stream.write_error) {
                client_close(&cl->s.stream);
            }
        }
    }
}

void check_timeout(int timeout) {
    do {
        struct client *cl, *tmp;
        time_t now = time(0);
        list_for_each_entry_safe(cl, tmp, &cli_list, list)
        {
            if (now - cl->time_alive > timeout || now - cl->time_alive < -timeout) {
                dawnlog_info("Client: close client connection! timeout=%d\n", (int)(now - cl->time_alive));
                client_close(&cl->s.stream);
            }
        }
    } while (0);

    do {
        struct network_con_s *con, *tmp;
        time_t now = time(0);
        list_for_each_entry_safe(con, tmp, &tcp_sock_list, list)
        {
            if (now - con->time_alive > timeout || now - con->time_alive < -timeout) {
                dawnlog_info("Server: close client_to_server connection! timeout=%d\n", (int)(now - con->time_alive));
                tcp_con_close(con);
            }
        }
    } while (0);
}

struct network_con_s* tcp_list_contains_address(struct sockaddr_in entry) {
    struct network_con_s *con;

    dawnlog_debug_func("Entering...");

    list_for_each_entry(con, &tcp_sock_list, list)
    {
        if (entry.sin_addr.s_addr == con->sock_addr.sin_addr.s_addr && entry.sin_port == con->sock_addr.sin_port)
        {
            return con;
        }
    }
    return NULL;
}

void print_tcp_array() {
    struct network_con_s *con;
    dawnlog_debug("--------Connections------\n");
    list_for_each_entry(con, &tcp_sock_list, list)
    {
        dawnlog_debug("Connecting to Port: %d, Connected: %s\n", ntohs(con->sock_addr.sin_port), con->connected ? "True" : "False");
    }
    dawnlog_debug("------------------\n");
}
