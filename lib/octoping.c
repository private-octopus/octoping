/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#ifdef _WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <Windows.h>
#include <WS2tcpip.h>

#ifndef WSA_START
#define WSA_START(x, y) WSAStartup((x), (y))
#endif

#ifndef SOCKET_TYPE
#define SOCKET_TYPE SOCKET
#endif

#ifndef SOCKET_CLOSE
#define SOCKET_CLOSE(x) closesocket(x)
#endif

#ifndef SOCKLEN_T
#define SOCKLEN_T int
#endif

#else /* Linux */

#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __USE_XOPEN2K
#define __USE_XOPEN2K
#endif
#ifndef __USE_POSIX
#define __USE_POSIX
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>

#define SERVER_CERT_FILE "certs/cert.pem"
#define SERVER_KEY_FILE "certs/key.pem"

#ifndef SOCKET_TYPE
#define SOCKET_TYPE int
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET -1
#endif
#ifndef SOCKET_CLOSE
#define SOCKET_CLOSE(x) close(x)
#endif
#ifndef WSA_LAST_ERROR
#define WSA_LAST_ERROR(x) ((long)(x))
#endif
#ifndef SOCKLEN_T
#define SOCKLEN_T socklen_t
#endif
#endif

/*
* By default, the server uses the port number 0xc389 (50057).
* The value 0xc389 corresponds to the first 4 digits of the
* MD5 hash of the string "octoping", which is:
* 0xc3896939402e97b40501795bff15584d. The client uses a
* randomly assigned port number.
* The listening port value can be set with the command line
* option [-p port].
*/
#define OCTOPING_PORT 0xc389

/*
 * Provide clock time
 */
uint64_t current_time()
{
    uint64_t now;
#ifdef WIN32
    FILETIME ft;
    /*
    * The GetSystemTimeAsFileTime API returns  the number
    * of 100-nanosecond intervals since January 1, 1601 (UTC),
    * in FILETIME format.
    */
    GetSystemTimePreciseAsFileTime(&ft);

    /*
    * Convert to plain 64 bit format, without making
    * assumptions about the FILETIME structure alignment.
    */
    now = ft.dwHighDateTime;
    now <<= 32;
    now |= ft.dwLowDateTime;
    /*
    * Convert units from 100ns to 1us
    */
    now /= 10;
    /*
    * Account for microseconds elapsed between 1601 and 1970.
    */
    now -= 11644473600000000ULL;
#else
    struct timeval tv;
    (void)gettimeofday(&tv, NULL);
    now = (tv.tv_sec * 1000000ull) + tv.tv_usec;
#endif
    return now;
}

typedef struct st_octoping_options_t {
    char const* server_name;
    uint16_t server_port;
    uint16_t source_port;
    unsigned int is_server : 1;
    unsigned int real_time : 1;
    uint64_t interval_us;
    uint64_t duration_us;
    char const* file_name;
} octoping_options_t;

static void usage(char const * sample_name)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "    %s [-r] [-p port] [-o file_name] <server_name> <server_port> <interval_ms> <duration_seconds>", sample_name);
    fprintf(stderr, "or :\n");
    fprintf(stderr, "    %s [-r] [-p port]", sample_name);
    fprintf(stderr, "use -r to request real time enhancements from the OS.");
    fprintf(stderr, "use -p to set the local source port number.");
    fprintf(stderr, "use -o to sdirect output to file instead of stdout.");
    exit(1);
}

int parse_options(octoping_options_t * options, int argc, char** argv)
{
    int ret = 0;
    int option_index = 1;

    memset(options, 0, sizeof(octoping_options_t));

    /* first parse the optional values. */
    while (option_index < argc && ret == 0) {
        char const* option_value = argv[option_index];

        if (strcmp(option_value, "-r") == 0) {
            options->real_time = 1;
            option_index++;
        }
        else if (strcmp(option_value, "-p") == 0) {
            option_index++;
            if (option_index >= argc) {
                fprintf(stderr, "Port value not set");
                ret = -1;
            }
            else {
                int source_port = atoi(argv[option_index]);
                if (source_port < 0 || source_port > 0xffff) {
                    fprintf(stderr, "Invalid source port: %s\n", argv[option_index]);
                    ret = -1;
                }
                else {
                    options->source_port = (uint16_t)source_port;
                    option_index++;
                }
            }
        }
        else if (strcmp(option_value, "-f") == 0) {
            option_index++;
            if (option_index >= argc) {
                fprintf(stderr, "Port value not set");
                ret = -1;
            }
            else {
                options->file_name = argv[option_index];
                option_index++;
            }
        } else {
            /* end of optional parameters */
            break;
        }
    }
    if (ret == 0) {
        if (option_index >= argc) {
            options->is_server = 1;
        }
        else if (option_index + 4 != argc) {
            fprintf(stderr, "Invalid client specification\n");
            ret = -1;
        }
        else {
            int server_port = atoi(argv[option_index + 1]);
            int interval_ms = atoi(argv[option_index + 2]);
            int seconds = atoi(argv[option_index + 3]);
            uint64_t interval_us = 0;
            uint64_t duration = 0;

            if (server_port < 0 || server_port > 0xffff) {
                fprintf(stderr, "Invalid server port: %s\n", argv[option_index + 1]);
                ret = -1;
            }
            if (interval_ms <= 0) {
                printf("Invalid interval in milliseconds: %s\n", argv[option_index + 2]);
                ret = -1;
            } else if (seconds <= 0) {
                printf("Invalid duration in seconds: %s\n", argv[option_index + 3]);
                ret = -1;
            }
            else {
                options->server_name = argv[option_index];
                options->server_port = (uint16_t)server_port;
                options->interval_us = ((uint64_t)interval_ms) * 1000;
                options->duration_us = ((uint64_t)seconds) * 1000000;
            }
        }
    }

    return ret;
}

int get_port(char const* sample_name, char const* port_arg)
{
    int server_port = atoi(port_arg);
    if (server_port <= 0) {
        fprintf(stderr, "Invalid port: %s\n", port_arg);
        usage(sample_name);
    }

    return server_port;
}

uint64_t parse_64(uint8_t* buffer)
{
    uint64_t x = 0;
    for (int i = 0; i < 8; i++) {
        x *= 256;
        x += buffer[i];
    }
    return x;
}

void marshall_64(uint8_t* buffer, uint64_t x)
{
    for (int i = 0; i < 8; i++) {
        buffer[i] = (uint8_t)((x >> (8 * (7 - i))) & 0xff);
    }
}

void network_error()
{
    int err = 0;
#ifdef _WINDOWS
    err = WSAGetLastError();
#else
    err = errno;
#endif
    printf("Network error: %d (0x%x)\n", err, err);
}

int octoping_server(int server_port)
{
    int ret = 0;
	uint8_t buffer[512];
    SOCKET_TYPE s = INVALID_SOCKET;
    struct sockaddr_in addr4 = { 0 };

    if (server_port == 0) {
        server_port = OCTOPING_PORT;
    }

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        ret = -1;
    }
    else {
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons((unsigned short)server_port);
        ret = bind(s, (struct sockaddr*)&addr4, sizeof(addr4));
        if (ret != 0) {
            printf("Bind returns %d\n", ret);
        }
        if (ret == 0) {
            printf("Octoping waiting for packets on port: %d\n", server_port);
        }
        while (ret == 0) {
            SOCKLEN_T from_len = (SOCKLEN_T) sizeof(addr4);
            int l = recvfrom(s, (char*)buffer, sizeof(buffer), 0, (struct sockaddr*)&addr4, &from_len);
            if (l < 0) {
                network_error();
                printf("Recvfrom returns %d\n", l);
                ret = -1;
            }
            else if (l >= 16) {
                marshall_64(buffer + 16, current_time());
                l = sendto(s, (char*)buffer, 24, 0,(const struct sockaddr*) &addr4, sizeof(addr4));
                if (l < 0) {
                    network_error();
                    printf("Sendto returns %d\n", l);
                    ret = -1;
                }
            }
        }
        SOCKET_CLOSE(s);
    }

    return ret;
}

int octoping_client(octoping_options_t * options)
{
    int ret = 0;
    uint8_t buffer[512];
    SOCKET_TYPE s = INVALID_SOCKET;
    struct sockaddr_in addr_to;
    struct sockaddr_in addr_from;
    FILE* F = NULL;
    int nb_pending = 0;
#define NUMBER_RANGE 1024
    uint64_t pending[NUMBER_RANGE];
    uint64_t basis = 0;
    uint64_t seqnum = 0;
    int64_t phase = INT64_MAX;

    memset(pending, 0, sizeof(pending));
    memset(&addr_to, 0, sizeof(addr_to));

    if (inet_pton(AF_INET, options->server_name, &addr_to.sin_addr) != 1){
        printf("%s is not a valid IPv4 address\n", options->server_name);
        ret = -1;
    }
    else {
        /* Valid IPv4 address */
        uint16_t source_port = (options->source_port == 0) ? OCTOPING_PORT : options->source_port;
        addr_to.sin_family = AF_INET;
        addr_to.sin_port = htons(source_port);

        printf("Will send packets to: %s\n", inet_ntop(AF_INET, &addr_to.sin_addr, (char*)buffer, sizeof(buffer)));

        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            ret = -1;
        }
        else {
            if (options->file_name == NULL) {
                F = stdout;
            }
            else {
#ifdef _WINDOWS
                errno_t err = fopen_s(&F, options->file_name, "wt");
                if (err != 0) {
                    if (F != NULL) {
                        fclose(F);
                        F = NULL;
                    }
                }
#else
                F = fopen(file_name, "wt");
#endif
                if (F == NULL) {
                    printf("Cannot open %s\n", options->file_name);
                    ret = -1;
                }
            }
            if (ret == 0) {
                uint64_t start_time = current_time();
                uint64_t next_send_time = start_time;
                uint64_t end_send_time = next_send_time + options->duration_us;
                uint64_t end_recv_time = end_send_time + 3000000;
                uint64_t t = start_time;
                uint64_t r_t = t + 1000000;
                uint64_t min_rtt = UINT64_MAX;

                if (fprintf(F, "number, sent, received, echo, rtt, up_t, down_t, phase\n") <= 0) {
                    printf("Cannot write first line on %s", options->file_name);
                    ret = -1;
                }
                while (ret == 0 && t < end_recv_time) {
                    if (t >= r_t) {
                        if (options->file_name != NULL) {
                            printf(".");
                            fflush(stdout);
                        }
                        fflush(F);
                        r_t += 1000000;
                    }
                    if (t >= next_send_time) {
                        int l;
                        marshall_64(buffer, seqnum);
                        marshall_64(buffer+8, t);
                        l = sendto(s, (char*)buffer, 16, 0, (struct sockaddr*)&addr_to, sizeof(addr_to));
                        if (l <= 0) {
                            network_error();
                            printf("Sendto returns %d\n", l);

                            ret = -1;
                        }
                        else {
                            if (seqnum >= basis + NUMBER_RANGE) {
                                basis = (seqnum / NUMBER_RANGE) * NUMBER_RANGE;
                            }
                            if (pending[seqnum - basis] != 0 && seqnum > NUMBER_RANGE) {
                                uint64_t missing = seqnum - NUMBER_RANGE;
                                double sent_sec = ((double)(pending[seqnum - basis] - start_time)) / 1000000.0;
                                (void)fprintf(F, "%"PRIu64",%"PRIi64",0,0,0,0,0,0\n", missing, pending[seqnum - basis] - start_time);
                            }
                            pending[seqnum - basis] = t;
                            seqnum ++;

                            while (next_send_time < t) {
                                next_send_time += options->interval_us;
                            }
                            if (next_send_time > end_send_time) {
                                next_send_time = end_recv_time;
                            }
                        }
                    }
                    else {
                        uint64_t delta_t = next_send_time - t;
                        fd_set readfds;
                        struct timeval tv = { 0 };
                        FD_ZERO(&readfds);
                        FD_SET(s, &readfds);
                        int selected;

                        tv.tv_sec = (long)(delta_t / 1000000);
                        tv.tv_usec = (long)(delta_t % 1000000);
                        selected = select((int)s + 1, &readfds, NULL, NULL, &tv);
                        if (selected < 0) {
                            network_error();
                            ret = -1;
                            printf("Error: select returns %d\n", selected);
                        } else if (selected > 0) {
                            SOCKLEN_T from_len = (int)sizeof(addr_from);
                            int l = recvfrom(s, (char*)buffer, sizeof(buffer), 0, (struct sockaddr*)&addr_from, &from_len);

                            if (l < 0) {
                                network_error();
                                printf("Error: recvfrom returns %d\n", l);
                                ret = -1;
                            } else if (l >= 24){
                                uint64_t r_seqnum;
                                uint64_t sent_at;
                                uint64_t recv_at;
                                uint64_t middle;
                                uint64_t echo_at = current_time();
                                int64_t sent_n = 0;
                                int64_t recv_n = 0;
                                int64_t echo_n = 0;
                                uint64_t rtt = 0;
                                int64_t up_t = 0;
                                int64_t down_t = 0;

                                r_seqnum = parse_64(buffer);
                                sent_at = parse_64(buffer+8);
                                recv_at = parse_64(buffer+16);

                                if (sent_at < echo_at) {
                                    rtt = echo_at - sent_at;
                                    middle = (echo_at + sent_at) / 2;
                                    if (phase == INT64_MAX) {
                                        phase = recv_at - middle;
                                        min_rtt = rtt;
                                    }
                                    else {
                                        if (rtt < min_rtt) {
                                            min_rtt = rtt;
                                        }
                                        if (rtt < (min_rtt + min_rtt / 8)) {
                                            phase = (7 * phase + recv_at - middle) / 8;
                                        }
                                    }
                                    up_t = (recv_at - phase) - sent_at;
                                    down_t = rtt - up_t;
                                    if (up_t < 0 || down_t < 0) {
                                        phase = recv_at - middle;
                                        up_t = rtt / 2;
                                        down_t = rtt - up_t;
                                    }
                                }
                                sent_n = sent_at - start_time;
                                recv_n = recv_at - start_time;
                                down_t = echo_at - start_time;
                                if (r_seqnum >= seqnum) {
                                    printf("Received number %" PRIu64 " while next number to send is %" PRIu64 "\n",
                                        r_seqnum, seqnum);
                                    ret = -1;
                                }
                                else if (fprintf(F, "%"PRIu64",%"PRId64",%"PRId64",%"PRId64",%"PRIu64",%"PRId64", %"PRId64", %"PRId64"\n",
                                    r_seqnum, sent_n, recv_n, echo_n, rtt, up_t, down_t, phase) < 0) {
                                    printf("write on %s returns error", options->file_name);
                                    ret = -1;
                                }
                                else {
                                    if (r_seqnum >= basis) {
                                        pending[r_seqnum - basis] = 0;
                                    }
                                    else {
                                        if (seqnum - r_seqnum <= NUMBER_RANGE) {
                                            pending[r_seqnum + NUMBER_RANGE - basis] = 0;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    t = current_time();
                }
                printf("\n");

                /* Notice whatever is not yet echoed */
                for (uint64_t i = 0; ret == 0 && i < NUMBER_RANGE; i++) {
                    if (pending[i] != 0) {
                        uint64_t missing = basis + i;
                        double sent_sec = ((double)(pending[i] - start_time)) / 1000000.0;
                        if (missing >= seqnum) {
                            missing -= NUMBER_RANGE;
                        }
                        if (fprintf(F, "%"PRIu64",%"PRIi64",0,0,0,0,0, 0\n", missing, pending[i] - start_time) <= 0) {
                            printf("Cannot report missing packet #%" PRIu64 "\n", missing);
                            ret = -1;
                        }
                    }
                }

                if (options->file_name != NULL) {
                    (void)fclose(F);
                }
            }
        }
        SOCKET_CLOSE(s);
    }
    return ret;
}

int main(int argc, char** argv)
{
    int exit_code = 0;
    octoping_options_t options;
#ifdef _WINDOWS
    WSADATA wsaData = { 0 };
    (void)WSA_START(MAKEWORD(2, 2), &wsaData);
#endif

    if (parse_options(&options, argc, argv) != 0){
        usage(argv[0]);
    }
    else
    {
        if (options.real_time) {
            /* set the real time option */
        }
        if (options.is_server) {
            exit_code = octoping_server(options.source_port);
        }
        else {
            exit_code = octoping_client(&options);
        }
    }
    exit(exit_code);
}



