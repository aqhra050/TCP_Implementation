#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>

#include "common.h"
#include "packet.h"


/*
 * You are required to change the implementation to support
 * window size greater than one.
 * In the current implementation the window size is one, hence we have
 * only one send and receive packet
 */
tcp_packet *recvpkt;
tcp_packet *sndpkt;

//This is the next expect_sequence_number
int expected_sequence_number = 0;
//This is the acknowledgement number to be broadcasted
int acknowledgement_number = 0;

//Creating the PACKET_BUFFER and setting the window_cursors
tcp_packet* PACKET_BUFFER[BUFFER_SIZE];
int window_start = 0;
int window_end = WINDOW_SIZE;

int main(int argc, char **argv) {

    //Initiallizing the PACKET_BUFFER
    for (int i = 0; i < BUFFER_SIZE; i++)
        PACKET_BUFFER[i] = NULL;

    int sockfd; /* socket */
    int portno; /* port to listen on */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];
    struct timeval tp;

    //A flag to denote whether or not the file has been completely recieved
    char isFileRecieved = 0;

    /* 
     * check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets 
     * us rerun the server immediately after we kill it; 
     * otherwise we have to wait about 20 secs. 
     * Eliminates "ERROR on binding: Address already in use" error. 
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);

    unsigned int recievedPacketSequenceNumber;
    unsigned int bufferPlacementIndex;
    tcp_packet* windowStartPacketPtr;

    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        //VLOG(DEBUG, "waiting from server \n");
        //Recieving an acknowledgement
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        printf("DATA = %p\n", (void *) recvpkt->data);
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        //The EOF file packet would have a data size value of 0 

        recievedPacketSequenceNumber = recvpkt->hdr.seqno;

        // Finding the index at which we are to place the packet that is to be buffered 
        bufferPlacementIndex = (window_start+(recievedPacketSequenceNumber - expected_sequence_number) / DATA_SIZE) % BUFFER_SIZE;
        printf("recievedPacketSequenceNumber: %6d, expected_sequence_number: %6d \n", recievedPacketSequenceNumber
            , expected_sequence_number);
        //printf("DATA_SIZE: %6ld, BUFFER_SIZE: %6d \n", DATA_SIZE, BUFFER_SIZE);

        //Now we have appropriately placed the data of the packet onto the buffer
        PACKET_BUFFER[bufferPlacementIndex] = recvpkt;
        // memcpy(&PACKET_BUFFER[bufferPlacementIndex]->hdr, &recvpkt->hdr, sizeof(TCP_HDR_SIZE));
        printf("BUFFERED SEQ_NO %6d INTO %3d \n", PACKET_BUFFER[bufferPlacementIndex]->hdr.seqno, bufferPlacementIndex);

        // The Packet that is at the start of the window
        windowStartPacketPtr = PACKET_BUFFER[window_start];
        // While this packet is not null and we were waiting for this packet we accept it
        while (windowStartPacketPtr != NULL && windowStartPacketPtr->hdr.seqno == expected_sequence_number) {
            printf("ACCEPTED %6d\n", windowStartPacketPtr->hdr.seqno);
            acknowledgement_number = expected_sequence_number + windowStartPacketPtr->hdr.data_size;
            // To check for end of file packet
            if (windowStartPacketPtr->hdr.data_size == 0) {
                VLOG(INFO, "End Of File has been reached");
                isFileRecieved = 1;
                fclose(fp);
                break;
            }
            //Writing out the Accepted Packet into the File
            fseek(fp, windowStartPacketPtr->hdr.seqno, SEEK_SET);
            fwrite(windowStartPacketPtr->data, 1, windowStartPacketPtr->hdr.data_size, fp);
            expected_sequence_number = acknowledgement_number;

            // Modifying cursors to current state
            window_start = (window_start + 1) % BUFFER_SIZE;
            window_end = (window_end + 1) % BUFFER_SIZE;
            windowStartPacketPtr = PACKET_BUFFER[window_start];
            printf("windowStartPacketPtr: %p isNotNull=%d\n", windowStartPacketPtr, windowStartPacketPtr != NULL);
            printf("Post Acceptance [%3d, %3d) \n",window_start, window_end);
        }

        /* 
         * sendto: ACK back to the client 
         */
        gettimeofday(&tp, NULL);
        VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
        // Here we just send out an acknowledgement in response to the packet which we just recieved, either it had been something we expected, 
        // so that we acknowledge its sequence number or it is not what we had expected and we tell the sender where it is that we stand
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = acknowledgement_number;
        sndpkt->hdr.ctr_flags = ACK;
        printf("ACK SENT %6d\n", acknowledgement_number);
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
        //After recieving the entire file, we simply break from the loop and conclude matters
        if (isFileRecieved) {
            break;
        }
    }

    return 0;
}