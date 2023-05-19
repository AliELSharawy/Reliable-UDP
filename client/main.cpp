#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <netdb.h>
#include <string>
#include <bits/stdc++.h>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>

using namespace std;

static const int MSS = 508;

struct data_packet {
    uint16_t check_sum;
    uint16_t packet_length;
    uint32_t seq_no;
    char data [500];
};


struct ack_packet {
    uint16_t check_sum;
    uint16_t packet_length;
    uint32_t ack_no;
};

data_packet create_file_req_packet(const string& file_name);
void send_acknowledgement_packet(int client_socket, struct sockaddr_in server_address ,  int seqNum);
vector<string> read_args();
uint16_t get_data_checksum (const string& content, uint16_t packet_length , uint32_t seq_no);
uint16_t get_ack_checksum (uint16_t len , uint32_t ack_no);

int main()
{
    // read file_args
    vector<string> args = read_args();
    string IP_Address = args[0];
    int port = stoi(args[1]);
    string fileName = args[2];

    int client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(client_socket < 0) {
        perror("failed to create the client socket");
        exit(1);
    }

    struct sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    // initially send data packet with file name
    cout << "File Name is : " << fileName << " The length of the file : " << fileName.size() << endl;
    struct data_packet fileName_packet = create_file_req_packet(fileName);
    char* buffer = new char[MSS];
    memset(buffer, 0, MSS);
    memcpy(buffer, &fileName_packet, sizeof(fileName_packet));
    ssize_t bytesSent = sendto(client_socket, buffer, MSS, 0, (struct sockaddr *)&server_address, sizeof(struct sockaddr));
    if (bytesSent == -1) {
        perror("error, failed to send file name ");
        exit(1);
    }
    else{
       cout << "Client sent file Name " << fileName << endl;
    }

    char received_file_buffer[MSS];
    socklen_t addr_len = sizeof(server_address);
    ssize_t Received_bytes = recvfrom(client_socket, received_file_buffer, MSS, 0, (struct sockaddr*)&server_address, &addr_len);
    if (Received_bytes < 0){
        perror("error in receiving file_name ack .");
        exit(1);
    }

    auto* ack_Packet = (struct ack_packet*) received_file_buffer;
    cout << "ack packet length" << ack_Packet->packet_length << endl;
    long ack_packet_Length = ack_Packet->packet_length;
    string ack_file_contents [ack_packet_Length];
    bool rec[ack_packet_Length] = {false};
    int expectedSeqNum = 0;
    int i = 1;

    // receive all file packets
    while (i <= ack_packet_Length){
        memset(received_file_buffer, 0, MSS);
        ssize_t bytesReceived = recvfrom(client_socket, received_file_buffer, MSS, 0, (struct sockaddr*)&server_address, &addr_len);
        if (bytesReceived == -1){
            perror("Error receiving data data_packet.");
            break;
        }
        auto* data_packet = (struct data_packet*) received_file_buffer;
        cout <<"data_packet "<< i <<" received with seq_no: " << data_packet->seq_no <<endl;
        int received_packet_length = data_packet->packet_length;
        for (int j = 0 ; j < received_packet_length ; j++){
            ack_file_contents[data_packet->seq_no] += data_packet->data[j];
        }
        if (get_data_checksum(ack_file_contents[data_packet->seq_no], data_packet->packet_length, data_packet->seq_no) != data_packet->check_sum){
            cout << "corrupted data_packet: " << i << endl;
        }
        send_acknowledgement_packet(client_socket, server_address , data_packet->seq_no);
        i++;
    }

    // re-build packets and save it to file
    string content;
    for (int i = 0; i < ack_packet_Length ; i++)
        content += ack_file_contents[i];
    ofstream f_stream(fileName.c_str());
    f_stream.write(content.c_str(), content.length());
    cout << fileName << " is saved successfully . " << endl;
    return 0;
}

data_packet create_file_req_packet(const string& file_name) {
    // file_name is initial packet with only data: file name
    struct data_packet dataPacket{};
    strcpy(dataPacket.data, file_name.c_str());
    dataPacket.seq_no = 0;
    dataPacket.check_sum = 0;
    dataPacket.packet_length = file_name.length() + sizeof(dataPacket.check_sum) + sizeof(dataPacket.packet_length) + sizeof(dataPacket.seq_no);
    return dataPacket;
}

void send_acknowledgement_packet(int client_socket, struct sockaddr_in server_address, int seqNum){
    struct ack_packet ackPacket{};
    ackPacket.ack_no = seqNum;
    // ack packet doesn't have data so calc check sum on packet length and ack_no
    ackPacket.check_sum = get_ack_checksum(ackPacket.packet_length, ackPacket.ack_no);
    ackPacket.packet_length = sizeof(ackPacket);
    char* ack_buffer = new char[MSS];
    memset(ack_buffer, 0, MSS);
    memcpy(ack_buffer, &ackPacket, sizeof(ackPacket));
    ssize_t bytesSent = sendto(client_socket, ack_buffer, MSS, 0, (struct sockaddr *)&server_address, sizeof(struct sockaddr));
    if (bytesSent == -1){
        perror("error, can not send ack packet");
        exit(1);
    }
    else
        cout << "successfully sent Ack " << seqNum << endl;
}

vector<string> read_args(){
    string fileName = "info.txt";
    vector<string> args;
    string arg;
    ifstream file_args;
    file_args.open(fileName);
    while(getline(file_args, arg)){
        args.push_back(arg);
    }
    return args;
}

uint16_t get_data_checksum (const string& content, uint16_t packet_length , uint32_t seq_no){
    // sum: sum all data packet in byte
    uint32_t sum = 0;
    sum = packet_length + seq_no;
    char arr[content.length() + 1];
    strcpy(arr, content.c_str());
    for (int i = 0; i < content.length(); i++)
        sum += arr[i];
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    auto sum_complement = (uint16_t) (~sum);
    return sum_complement;
}

uint16_t get_ack_checksum (uint16_t len , uint32_t ack_no){
    // sum: sum all ack packet in byte
    uint32_t sum = 0;
    sum = len + ack_no;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    auto sum_complement = (uint16_t) (~sum);
    return sum_complement;
}

