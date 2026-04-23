/**
 * Echipa 11
 * IR3 2026
 * Proiect PCD - Helperi I/O pentru protocolul binar (framing)
 * Ce face modulul:
 * - ofera functii de citire/scriere completa pe socket (nu se pierd bytes)
 * - serializeaza/deserializeaza header-ul protocolului cu conversie endianness
 * - partajat intre server si client (ambii au nevoie de aceleasi operatii)
 * Erori tratate explicit:
 * - recv/send partial (bucla pana se transfera toti bytes)
 * - peer closed (recv returneaza 0)
 * - semnal intrerupt (EINTR -> reincerc)
 */

#include "proto.h" // definitiile protocolului (header, payload-uri, constante)

#include <errno.h> // errno, EINTR pt retry dupa semnale
#include <netinet/in.h> // htonl/ntohl pt conversie network <-> host byte order
#include <stddef.h> // size_t
#include <sys/socket.h> // recv, send pt transfer pe socket
#include <sys/types.h> // ssize_t

// citeste exact len bytes de pe socket; reincearca daca recv e partial sau intrerupt
int proto_read_full(int sock, void *buf, size_t len)
{
    unsigned char *cursor = (unsigned char *)buf;
    size_t got = 0;

    while (got < len) {
        ssize_t nread = recv(sock, cursor + got, len - got, 0);
        if (nread == 0) {
            return -1; // peer-ul a inchis conexiunea
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue; // intrerupt de semnal, reincerc
            }
            return -1; // eroare reala de I/O
        }
        got += (size_t)nread;
    }
    return 0;
}

// scrie exact len bytes pe socket; reincearca daca send e partial sau intrerupt
int proto_write_full(int sock, const void *buf, size_t len)
{
    const unsigned char *cursor = (const unsigned char *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t nwritten = send(sock, cursor + sent, len - sent, 0);
        if (nwritten <= 0) {
            if (nwritten < 0 && errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)nwritten;
    }
    return 0;
}

// citeste un header de protocol si converteste din network in host byte order
int proto_read_header(int sock, proto_header_t *hdr)
{
    proto_header_t net;
    if (proto_read_full(sock, &net, sizeof(net)) < 0) {
        return -1;
    }
    hdr->msg_size = ntohl(net.msg_size);
    hdr->client_id = ntohl(net.client_id);
    hdr->op_id = ntohl(net.op_id);
    hdr->task_id = ntohl(net.task_id);
    return 0;
}

// scrie un header de protocol convertind din host in network byte order
int proto_write_header(int sock, const proto_header_t *hdr)
{
    proto_header_t net;
    net.msg_size = htonl(hdr->msg_size);
    net.client_id = htonl(hdr->client_id);
    net.op_id = htonl(hdr->op_id);
    net.task_id = htonl(hdr->task_id);
    return proto_write_full(sock, &net, sizeof(net));
}

/*
 *> Compilare si exemple de rulare:
 *
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ make all
 * (proto.c este compilat si linkat atat in vps_server cat si in vps_client)
 * gcc -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wpedantic -Werror -g -Iinclude -c src/proto.c -o build/proto.o
 *
 * --- Exercitare cu succes ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o trim -i data/uploads/clip1.mp4 -O data/outputs/final_trimmed.mp4 -s 0 -e 1200
 * reply: task_id=17918 state=2 path=data/outputs/final_trimmed.mp4
 * (schimbul de header + payload dintre client si server trece prin helperii din proto.c)
 *
 * --- Exercitare cu esec ---
 * vladb:~/PCD/pcd-lucru/Proiect/skeleton$ bin/vps_client -o trim -i data/uploads/clip1.mp4 -O data/outputs/out.mp4 -s 0 -e 1000
 * client: cannot connect to 127.0.0.1:18081 (Connection refused)
 * (cand serverul nu ruleaza, helperii nu mai ajung sa transfere header/payload)
 */
