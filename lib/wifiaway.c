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

int parse_time_message()
{
    return 0;
}


static void usage(char const * sample_name)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "    %s client <server_name> <port> <interval_ms> <duration_seconds>", sample_name);
    fprintf(stderr, "or :\n");
    fprintf(stderr, "    %s server <port>", sample_name);
    exit(1);
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

int wifiaway_server(int server_port)
{
    int ret = 0;
	uint8_t buffer[512];
    SOCKET_TYPE s = INVALID_SOCKET;
    struct sockaddr_in addr4 = { 0 };

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

int wifiaway_client(char const* server, int server_port, uint64_t interval_us, uint64_t duration_us, char const * file_name)
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

    memset(pending, 0, sizeof(pending));
    memset(&addr_to, 0, sizeof(addr_to));

    if (inet_pton(AF_INET, server, &addr_to.sin_addr) != 1){
        printf("%s is not a valid IPv4 address\n", server);
        ret = -1;
    }
    else {
        /* Valid IPv4 address */
        addr_to.sin_family = AF_INET;
        addr_to.sin_port = htons((unsigned short)server_port);
        
        printf("Will send packets to: %s\n", inet_ntop(AF_INET, &addr_to.sin_addr, (char*)buffer, sizeof(buffer)));

        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) {
            ret = -1;
        }
        else {
#ifdef _WINDOWS
            errno_t err = fopen_s(&F, file_name, "wt");
            if (err != 0){
                if (F != NULL) {
                    fclose(F);
                    F = NULL;
                }
            }
#else
            F = fopen(file_name, "wt");
#endif
            if (F == NULL) {
                printf("Cannot open %s\n", file_name);
                ret = -1;
            }
            else {
                uint64_t start_time = current_time();
                uint64_t next_send_time = start_time;
                uint64_t end_send_time = next_send_time + duration_us;
                uint64_t end_recv_time = end_send_time + 3000000;
                uint64_t t = current_time();

                if (fprintf(F, "number, sent, received, echo\n") <= 0) {
                    printf("Cannot write first line on %s", file_name);
                    ret = -1;
                }
                while (ret == 0 && t < end_recv_time) {
                    printf("Loop,  %" PRIu64 "\n", t - start_time);
                    if (t >= next_send_time) {
                        int l;
                        printf("Send at %" PRIu64 "\n", t - start_time);
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

                                (void)fprintf(F, "%"PRIu64",%"PRIu64",0,0\n", missing, pending[seqnum - basis]);
                            }
                            pending[seqnum - basis] = t;
                            seqnum ++;

                            while (next_send_time < t) {
                                next_send_time += interval_us;
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
                            int from_len = (int)sizeof(addr_from);
                            int l = recvfrom(s, (char*)buffer, sizeof(buffer), 0, (struct sockaddr*)&addr_from, &from_len);

                            if (l < 0) {
                                network_error();
                                printf("Error: recvfrom returns %d\n", l);
                                ret = -1;
                            } else if (l >= 24){
                                uint64_t r_seqnum;
                                uint64_t sent_at;
                                uint64_t recv_at;
                                uint64_t echo_at = current_time();

                                r_seqnum = parse_64(buffer);
                                sent_at = parse_64(buffer+8);
                                recv_at = parse_64(buffer+16);

                                if (r_seqnum >= seqnum) {
                                    printf("Received number %" PRIu64 " while next number to send is %" PRIu64 "\n",
                                        r_seqnum, seqnum);
                                    ret = -1;
                                }
                                else if (fprintf(F, "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n", r_seqnum, sent_at, recv_at, echo_at) < 0) {
                                    printf("write on %s returns error", file_name);
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

                /* Notice whatever is not yet echoed */
                for (uint64_t i = 0; ret == 0 && i < NUMBER_RANGE; i++) {
                    if (pending[i] != 0) {
                        uint64_t missing = basis + i;
                        if (missing >= seqnum) {
                            missing -= NUMBER_RANGE;
                        }
                        if (fprintf(F, "%"PRIu64",%"PRIu64"0,0\n", missing, pending[i]) <= 0) {
                            printf("Cannot report missing packet #%" PRIu64 "\n", missing);
                            ret = -1;
                        }
                    }
                }
                (void)fclose(F);
            }
        }
        SOCKET_CLOSE(s);
    }
    return ret;
}

int main(int argc, char** argv)
{
    int exit_code = 0;
#ifdef _WINDOWS
    WSADATA wsaData = { 0 };
    (void)WSA_START(MAKEWORD(2, 2), &wsaData);
#endif

    if (argc < 2) {
        usage(argv[0]);
    }
    else if (strcmp(argv[1], "client") == 0) {
        if (argc < 6) {
            usage(argv[0]);
        }
        else {
            int server_port = get_port(argv[0], argv[3]);
            int interval_ms = atoi(argv[4]);
            int seconds = atoi(argv[5]);
            uint64_t interval_us = 0;
            uint64_t duration = 0;

            if (interval_ms <= 0) {
                printf("Invalid interval in milliseconds: %s\n", argv[4]);
                usage(argv[0]);
            } else if (seconds <= 0) {
                printf("Invalid duration in seconds: %s\n", argv[5]);
                usage(argv[0]);
            }
            else {
                interval_us = (uint64_t)interval_ms;
                interval_us *= 1000;
                duration = (uint64_t)seconds;
                duration *= 1000000;

                exit_code = wifiaway_client(argv[2], server_port, interval_us, duration, "test.csv");
            }
        }
    }
    else if (strcmp(argv[1], "server") == 0) {
        if (argc < 3) {
            usage(argv[0]);
        }
        else {
            int server_port = get_port(argv[0], argv[2]);
            exit_code = wifiaway_server(server_port);
        }
    }
    else
    {
        usage(argv[0]);
    }

    exit(exit_code);
}



