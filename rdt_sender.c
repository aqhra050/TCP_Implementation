#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD    0
#define RETRY  250 //millisecond
// #define RETRANSMIT_DELAY 2000 // millisecond

// Upper and lower bounds of RTO to prevent unrecoverable situations
#define MAX_RTO 2500
#define MIN_RTO 100

#define MAX(a,b) (((a)>(b))?(a):(b));

//Is the next_sequence number when buffering in the sender
int next_seqno=0;
//Is the base from which we begin sending
int send_base=0;

//These are timing structures used to measure the time elapsed between one send and the next (used to calculate sample rtt)
//The latter is used to deliniate a delay between fast retransmit sending
struct timeval initialTime, sendStartTime, sendEndTime, fastRetransmitStartTime, fastRetransmitEndTime;

//The output file
FILE* outputfilePTR = NULL;

//From the starter code
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;

//These will be used to adjust the window sizes
double cwnd = 1;
int ssthresh = 64;

//This will be used to store the packets that have been prepared for sending
tcp_packet* PACKET_BUFFER[BUFFER_SIZE];
//This is the spread between window_begin and end
//This will delineate the start of the window
int window_begin = 0;
//This will delineate the end of the window, it will be one initially since the initial value of cwnd is 1
int window_end = 1;
//This will delineate the present packet index that is to be read in
int read_cursor = 0;
//This will delineate the present packet index that is to be sent out
int send_cursor = 0;
//This will store the present packet index for which we are expect an acknowledgement
int unacked_cursor = 0;

//This will be used to delineate whether the present phase is that of slow-start, if it is not that of slow start 
//then it must be necessaraily that of congestion avoidance
int isSlowStart = 1; 

//These are used for the calculation of the dynamic Round-Trip Timeout
#define ALPHA 0.125
#define BETA 0.25
unsigned long rto = RETRY;
double estimated_rtt = 0.0;
double dev_rtt = 0.0;
double sample_rtt = 0.0;

//This is ACK we hope to recieve for
int pendingAcknowledgementCount = 0;

//This is used to record, the cwnd (as an int and as a float), ssthresh, and rto
void logger_file();

//This is a helper function that reads in the next data from the file fp, and stores it temproraily into a data buffer before returning a newly created packet and modifying appropiately the pointers, and will also set the end of file flag
tcp_packet* readAndCreatePacket(FILE* fp, char DATA_BUFFER[], int* send_base_ptr, int* next_seqno_ptr, char* isEndOfFilePTR);

//From Starter Code
void start_timer();

//From Starter Code
void stop_timer();

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */
void init_timer(unsigned long delay, void (*sig_handler)(int));


//This is a helper function that resends all data from the unacked_cursor (longest unacked packet) to the send cursor (the next packet to be sent), effectivly it resends all the packets presently in flight
void resend_packets(int sig);

int main (int argc, char **argv)
{
    int portno;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }
    fseek(fp, 0, SEEK_SET);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //All preceeding code is from the Starter Code

    //Inital decleration of the timeout
    init_timer(rto, resend_packets);
    next_seqno = 0;

    // Opening file for output
    outputfilePTR = fopen("CWND.csv", "w");

    //These are two flags used to maintain whether the timer is presently active or not (useful for first starting the timer and resetting the timer upon acks), and whether the end of file has been or has not been reached to send a blank packet
    char isTimerActive = 0;
    char isEndOfFile = 0;

    //This is the acknowledgement we hope to recieve, that which he have recieved, and the prev one
    int pendingAcknowledgement;
    int recievedAcknowledgement;
    int prevAcknowledgment = -1; // Dummy Value

    //This is to maintain the count of how many times we have measured the acknowledgement if repeated 
    int duplicateAcknowledgmentCount = 0;

    //This is to allow better freeng of memory
    for (int i = 0; i < BUFFER_SIZE; i++)
        PACKET_BUFFER[i] = NULL;

    //Start of the Initial Time
    gettimeofday(&initialTime, NULL);
    // First sample of time for resending (fast retransmit), we will only retransmit once every RETRANSMIT_DELAY seconds
    gettimeofday(&fastRetransmitStartTime, NULL);
    
    //This is to measure the time elapsed between entries into the fast retransmit 
    double retransmitTimeElapsed = 0;

    while (1)
    {
        printf("BEGIN READING: UNACKED_CURSOR %2d | [%2d, %2d] | SEND_CURSOR: %d | READ_CURSOR: %d\n", unacked_cursor, window_begin, window_end, send_cursor, read_cursor);
        //The Buffer Array of Packets upto window boundary will have some read from the file read, and stored as packets
        window_end = (int) (window_begin + floor(cwnd));
        while(read_cursor < window_end) {
            if (PACKET_BUFFER[read_cursor % BUFFER_SIZE] != NULL) {
                free(PACKET_BUFFER[read_cursor % BUFFER_SIZE]);
            }
            PACKET_BUFFER[read_cursor % BUFFER_SIZE] = readAndCreatePacket(fp, buffer, &send_base, &next_seqno, &isEndOfFile);
            printf("READING PACKET #%6d | BASE: %6d NEXT_SEQNO: %6d | [%2d, %2d]\n", read_cursor, send_base, next_seqno, window_begin, window_end);
            read_cursor = read_cursor + 1;
            //Checking if Blank Packet
            if(isEndOfFile) {
                break;
            }
        }
        printf("END READING: UNACKED_CURSOR %2d | [%2d, %2d] | SEND_CURSOR: %d | READ_CURSOR: %d\n", unacked_cursor, window_begin, window_end, send_cursor, read_cursor);
        //The Buffer Array of Packets upto window boundary (where till data has been read) will be sent out
        printf("BEGIN SENDING: UNACKED_CURSOR %2d | [%2d, %2d] | SEND_CURSOR: %d | READ_CURSOR: %d\n", unacked_cursor, window_begin, window_end, send_cursor, read_cursor);
        while (send_cursor < read_cursor) {
            VLOG(DEBUG, "SENDING PACKET #%2d | SEQ NO: %6d", send_cursor, PACKET_BUFFER[send_cursor % BUFFER_SIZE]->hdr.seqno);
            if(sendto(sockfd, PACKET_BUFFER[send_cursor% BUFFER_SIZE], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[send_cursor % BUFFER_SIZE]), 0, (const struct sockaddr *) &serveraddr, serverlen) < 0) {
                    error("sendto");
            }
            send_cursor = send_cursor + 1;

            //Beginning the timer if it is not presently active
            if (!isTimerActive) {
                start_timer();
                // Sending for the first time, and beginning measurement of time since send (to be used to measue RTT)
                gettimeofday(&sendStartTime, NULL);
                isTimerActive = 1;
                //printf("Activating Timer \n");
            }
        }

        printf("END SENDING: UNACKED_CURSOR %2d | [%2d, %2d] | SEND_CURSOR: %d | READ_CURSOR: %d\n", unacked_cursor, window_begin, window_end, send_cursor, read_cursor);

        //The ACKs are now to be considered, the acknowledgment number must acknowledge the sequence number of the present oldest packet yet to be acknowledged
        pendingAcknowledgement = PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno;
        //Recieving the acknoweledgement
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom");
        }
        recvpkt = (tcp_packet *)buffer;
        recievedAcknowledgement = recvpkt->hdr.ackno;
        assert(get_data_size(recvpkt) <= DATA_SIZE);
        printf("ACK RECIEVED %6d | PENDING SEQ %6d | PENDING ACK %6d\n", recievedAcknowledgement, pendingAcknowledgement, pendingAcknowledgement+PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.data_size);

        //If the presently received acknowledgement is not one that we have previously recieved, only then it could be potentially acknowledge something that is presently unacknowledged
        if (recievedAcknowledgement != prevAcknowledgment){
            prevAcknowledgment = recievedAcknowledgement;  
            duplicateAcknowledgmentCount = 0;
            //Used to break out of the while loop of reading, sending, waiting for acknowledgement when the end of file has been reached
            if (isEndOfFile) {
                printf("We are calling it a day\n");
                break;
            }
            printf("BEGAN RUNNING THROUGH ALL PACKETS IN FLIGHT FOR ACK\n");
            //Since we use cumulative acknowledgements, we must acknowledge more than the oldest unacked packet in flight, and as such we keep on changing the cursor for the unacked packet in flight whenever the acknowledgement number is greater than the present oldest unacked packet in flight's sequence numbers
            while (recievedAcknowledgement > pendingAcknowledgement) {
                //We are acknowledging the packet presently at the unacked cursor, and accordingly shifting the cursor and the window start
                unacked_cursor = unacked_cursor + 1;
                window_begin = window_begin + 1;
                //We must now change the cwnd value depending on the specific paradigm we are under

                //This is to prevent previously in flight packets that are presently in flight but expired from being counted towards the expansion of the cwnd
                if (pendingAcknowledgementCount > 0){
                    pendingAcknowledgementCount--;
                } else {
                    //Varying upon the specific paradigm, we have to adjust the cwnd
                    if (isSlowStart) {
                        //Increment the cwnd value appropiately in the case of slow start
                        cwnd += 1;
                        logger_file();
                        printf("SlowStart | CWND + %3.2f\n", cwnd);

                        //Should the cwnd reach sshtresh, we must shift to the other paradigm
                        if (cwnd >= ssthresh) {
                            cwnd=ssthresh;
                            isSlowStart = 0;
                        }
                    } else {
                        //the floor present boosted accumulation
                        cwnd += 1 / floor(cwnd);
                        logger_file();
                        printf("Con Avoid | CWND + %3.2f\n", cwnd);
                    }    
                }
                //the window_end is adjusted correctly
                window_end = (window_begin + (int) floor(cwnd));
                printf("ACK --> NEW UNACKED CURSOR %2d | BUFFER_SIZE %2d \n", unacked_cursor, BUFFER_SIZE);
                printf("ACK --> NEW WINDOW [%d, %d) | [%d, %d)\n", window_begin, window_end, window_begin%BUFFER_SIZE, window_end%BUFFER_SIZE);
                //This checks if we have acknowledged all the packets that we have read, if so we must do some more reading, at least until the next packet to be read is after the cursor
                if (unacked_cursor >=read_cursor) {
                    while(read_cursor < window_end) {
                        //Clearing out memory each time a new packet is created
                        if (PACKET_BUFFER[read_cursor % BUFFER_SIZE] != NULL) {
                            free(PACKET_BUFFER[read_cursor % BUFFER_SIZE]);
                        }
                        PACKET_BUFFER[read_cursor % BUFFER_SIZE] = readAndCreatePacket(fp, buffer, &send_base, &next_seqno, &isEndOfFile);
                        printf("Alternate | Read&Store Packet @%2d | BASE: %6d NEXT_SEQNO: %6d | [%2d, %2d]\n", read_cursor, send_base, next_seqno, window_begin, window_end);
                        read_cursor = read_cursor + 1;
                        //Checking if Blank Packet
                        if(isEndOfFile) {
                            break;
                        }
                    }
                }
                pendingAcknowledgement = PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno;
                printf("ACK --> NEW PENDING ACKNOWLEDGEMENT %6d\n", pendingAcknowledgement);
                //Timer is reset after each acknowledgement, so as to be assosciated with the new unacked cursor
                
                printf("Timer Activation Value: %d \n ", isTimerActive);
                if (isTimerActive) {
                    stop_timer();
                    //we have recieved an appropiate ack, one packet has done round trip
                    gettimeofday(&sendEndTime, NULL);
                    //Changing the timer
                    //We have to again prevent the inflight expired packets from screwing things up
                    if (pendingAcknowledgementCount > 0) {
                        pendingAcknowledgementCount--;
                    } else {
                        sample_rtt = ((double) sendEndTime.tv_sec - (double) sendStartTime.tv_sec)*1000 + ((double) sendEndTime.tv_usec - (double) sendStartTime.tv_usec)/1000;
                    }

                    estimated_rtt = MAX(((1.0 - (double) ALPHA) * estimated_rtt + (double) ALPHA * sample_rtt), 1.0);
                    dev_rtt = MAX(((1.0 - (double) BETA) * dev_rtt + (double) BETA * abs(estimated_rtt - sample_rtt)), 1.0);
                    
                    rto = MAX(floor(estimated_rtt + 4 * dev_rtt), 1);
                    //This is to prevent the rto becoming too large or too small, which has been a great pain in testing
                    rto = -1 * MAX(-1*MAX_RTO, -1 * rto);
                    rto = MAX(rto, MIN_RTO);

                    printf("\t RTO: %ld   EST: %f   DEV: %f  \n", rto, estimated_rtt, dev_rtt);

                    //Adjusting the timer
                    init_timer(rto, resend_packets);
                    start_timer();
                    //Starting the timer again, it will be stopped upon the ack
                    gettimeofday(&sendStartTime, NULL);
                }
            }
        printf("STOPPED RUNNING THROUGH ALL PACKETS IN FLIGHT FOR ACK\n");
 
        } else {
            //If the present function was the same as that previous, this is a duplicate
            duplicateAcknowledgmentCount++;
            // Now we will use this to compute the time elapsed since last "fast retransmit"
            gettimeofday(&fastRetransmitEndTime, NULL);
            retransmitTimeElapsed = ((double) fastRetransmitEndTime.tv_sec - (double) fastRetransmitStartTime.tv_sec)*1000 + ((double) fastRetransmitEndTime.tv_usec - (double) fastRetransmitStartTime.tv_usec)/1000;
            //A mechanism to prevent repeated repeated entry into retransmit, which would reduced to cwnd to one very quickly
            if ((duplicateAcknowledgmentCount == 2) && (retransmitTimeElapsed> cwnd*rto)) {
                ssthresh = MAX(cwnd/2, 2);
                cwnd = MAX(cwnd/2, 1);
                isSlowStart = 0;
                logger_file();
                printf("  DUP ACC | CWND - %3.2f\n", cwnd);
                duplicateAcknowledgmentCount = 0;

                // Resending lost packet
                VLOG(DEBUG, "Resending Packet %2d (%6d)to %s", unacked_cursor, PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
                if(sendto(sockfd, PACKET_BUFFER[unacked_cursor % BUFFER_SIZE], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]), 0, (const struct sockaddr *) &serveraddr, serverlen) < 0) {
                    error("sendto");
                }
                printf("RESENT PACKET WITH SEQ %6d \n", PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno);
                gettimeofday(&fastRetransmitStartTime, NULL);
                //Time beginning for retransmit
            }
        }
    }

    for (int i = 0; i < BUFFER_SIZE; i++)
        if (PACKET_BUFFER[i] != NULL)
            free(PACKET_BUFFER[i]);

    fclose(outputfilePTR);
    return 0;
}

//Logging the time stamp, floor of cwnd, the cwnd, the ssthreh and rto
void logger_file(){
    if (outputfilePTR!=NULL) {
        struct timeval presentTime;
        gettimeofday(&presentTime, NULL);
        double timeElapsed = ((double) presentTime.tv_sec - (double) initialTime.tv_sec) + ((double) presentTime.tv_usec - (double) initialTime.tv_usec)/ 1000000;
        fprintf(outputfilePTR,"%.6F, %d, %.2F, %d, %ld\n", timeElapsed, (int) floor(cwnd),cwnd, ssthresh, rto);
    }
}

//This is a helper function to create packets from file
tcp_packet* readAndCreatePacket(FILE* fp, char DATA_BUFFER[], int* send_base_ptr, int* next_seqno_ptr, char* isEndOfFilePTR) {
    int len = fread(DATA_BUFFER, 1, DATA_SIZE, fp);
    if (len <= 0)
    {
        //VLOG(INFO, "Creating a new Packet, End Of File has been reached");
        sndpkt = make_packet(0);
        *isEndOfFilePTR = 1;
    } else {
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, DATA_BUFFER, len);
    }
    *send_base_ptr = *next_seqno_ptr;
    *next_seqno_ptr = *send_base_ptr + len;

    sndpkt->hdr.seqno = send_base;

    return sndpkt;
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void init_timer(unsigned long delay, void (*sig_handler)(int)) 
{
    // printf("DELAY: %ld \n", delay);
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

//This will be called upon timeout
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //at this point, we will have window_end - unacked_cursor - 1 packets in flight, all of which for we can recieve acks
        //these acks will need to problems in calculation of sample_rtt, as such we disregard the first n - 1 acks as a heuristic when measuring sample rtt
        pendingAcknowledgementCount = send_cursor - unacked_cursor - 1;

        stop_timer();
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "TIMEOUT | PREV_RTO: %ld | ", rto);
        //Adjusting the rto, within bounds
        rto = rto * 2;
        rto = -1 * MAX(-1*MAX_RTO, -1 * rto);
        rto = MAX(rto, MIN_RTO);
        VLOG(INFO, "NEW_RTO: %ld\n", rto);

        //Activating slow start if not active
        if (isSlowStart) {
            //printf("slowStart | TIMEOUT\n");
        } else {
            //printf("congestionAvoidance | TIMEOUT\n");
            // Now we change from AIMD to Slow Start
            isSlowStart = 1;
        }

        // // Preparing to restart slow start with a modified upper threshold
        ssthresh = MAX(2, cwnd/2);
        cwnd = 1;
        logger_file();

        printf("TIMEOUT | CWND - %3.2f\n", cwnd);



        printf("BEGIN RESENDING: UNACKED_CURSOR %2d %6d %6d \n|\t [%2d, %2d] | SEND_CURSOR: %d | READ_CURSOR: %d\n", unacked_cursor, PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno, PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno + PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.data_size , window_begin, window_end, send_cursor, read_cursor);
        //The Buffer Array of Packets upto window boundary (where till data has been read) will be sent out
        int resend_cursor = unacked_cursor;
        while (resend_cursor < send_cursor) {
            //VLOG(DEBUG, "Resending Packet %2d (%6d)to %s", resend_cursor, PACKET_BUFFER[resend_cursor % BUFFER_SIZE]->hdr.seqno, inet_ntoa(serveraddr.sin_addr));
            if(sendto(sockfd, PACKET_BUFFER[resend_cursor % BUFFER_SIZE], TCP_HDR_SIZE + get_data_size(PACKET_BUFFER[resend_cursor % BUFFER_SIZE]), 0, (const struct sockaddr *) &serveraddr, serverlen) < 0) {
                    error("sendto");
            }
            resend_cursor = resend_cursor + 1;
        }

        // Because of Packege being resent, we restart timer
        init_timer(rto, resend_packets);
        start_timer();
        gettimeofday(&sendStartTime, NULL);
        //printf("END RESENDING: UNACKED_CURSOR %2d %6d %6d \n|\t [%2d, %2d] | SEND_CURSOR: %d | READ_CURSOR: %d\n", unacked_cursor, PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno, PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.seqno + PACKET_BUFFER[unacked_cursor % BUFFER_SIZE]->hdr.data_size , window_begin, window_end, send_cursor, read_cursor);
    }
}
