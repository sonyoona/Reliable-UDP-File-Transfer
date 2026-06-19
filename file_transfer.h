#ifndef FILE_TRANSFER_H
#define FILE_TRANSFER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define DATA_SIZE 1024
#define HEADER_SIZE ((int)(sizeof(int) * 3))

#define TYPE_DATA 1
#define TYPE_ACK  2
#define TYPE_FIN  3

#pragma pack(push, 1)
typedef struct {
    int type;
    int seq;
    int size;
    char data[DATA_SIZE];
} Packet;

typedef struct {
    int type;
    int seq;
} AckPacket;
#pragma pack(pop)

long long get_current_time_ms(void);

#endif