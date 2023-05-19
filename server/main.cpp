#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <string>
#include <cstring>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <chrono>
#include <bits/stdc++.h>

using namespace std;

static const int MSS = 508;
static const int ACK_PACKET_SIZE = 8;
static const int CHUNK_SIZE = 499;

struct lost_packet{
    int seq_no;
    chrono::time_point<chrono::system_clock> packet_timer;
};

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

void handle_client_request(int server_socket, int client_fd, sockaddr_in client_addr, char rec_buffer [] , int bufferSize);
long checkFileExistence(const string& fileName);
vector<string> readDataFromFile(const string& fileName);
void sendTheData_HandleCongestion (int client_fd, struct sockaddr_in client_addr , vector<string> data);
data_packet create_data_packet(const string& data, int seq_no);
bool send_packet(int client_fd, struct sockaddr_in client_addr , const string& data, int seq_no);
vector<string> readArgsFile();
bool DropTheDatagram();
uint16_t get_data_checksum (const string& content, uint16_t len , uint32_t seq_no);
uint16_t get_ack_checksum (uint16_t packet_length , uint32_t ack_no);

enum fsm_state {slow_start, congestion_avoidance, fast_recovery};
int port, RandomSeedGen;
double PLP;
vector<lost_packet> lost_packets;
vector<data_packet> sent_packets;

int main(){
    vector<string> args = readArgsFile();
    port = stoi(args[0]);
    RandomSeedGen = stoi(args[1]);
    PLP = stod(args[2]);
    srand(RandomSeedGen);

    int server_socket, client_socket;

    struct sockaddr_in server_address{};
    struct sockaddr_in client_address{};
    int server_addr_len = sizeof(server_address);

    if ((server_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0){
        perror("error creating server socket ! ");
        exit(1);
    }

//    memset(&server_address, 0, sizeof(server_address));
//    memset(&client_address, 0, sizeof(client_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    server_address.sin_addr.s_addr = INADDR_ANY;
//    memset(&(server_address.sin_zero), '\0', ACK_PACKET_SIZE);

    if (bind(server_socket, (struct sockaddr *) &server_address, sizeof(server_address)) < 0){
        perror("error, in binding server ! ");
        exit(1);
    }

    while (true){
        socklen_t client_addr_len = sizeof(struct sockaddr);
        cout << "Waiting For A New Connection ... " << endl;
        char rec_buffer[MSS];
        // receive file name from client
        ssize_t Received_bytes = recvfrom(server_socket, rec_buffer, MSS, 0, (struct sockaddr*)&client_address, &client_addr_len);
        if (Received_bytes <= 0){
             perror("error in receiving bytes of the file name !");
             exit(1);
        }
        /** forking to handle request **/
         pid_t pid = fork();
         if (pid == -1){
             perror("error in forking a child process for the client ! ");
         } else if (pid == 0){
            if ((client_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0){
                perror("error creating a socket for the client ! ");
                exit(1);
            }
            // child handle send file packets to client
             handle_client_request(server_socket,client_socket, client_address, rec_buffer , MSS);
             exit(0);
         }

    }
    close(server_socket);
    return 0;
}

void handle_client_request(int server_socket, int client_fd, struct sockaddr_in client_addr, char rec_buffer [] , int bufferSize) {
     auto* data_packet = (struct data_packet*) rec_buffer;
     string fileName = string(data_packet->data);
     cout << "requested file name from client  : " << fileName << " , Length : " << fileName.size() << endl;
     long fileSize = checkFileExistence(fileName);
     if (fileSize == -1){
        return;
     }
     int numberOfPackets = ceil(fileSize * 1.0 / CHUNK_SIZE);
     cout << "File Size : " << fileSize << " Bytes , Num. of chunks : " << numberOfPackets << endl;

     /** send ack to file name **/
     struct ack_packet ack{};
     ack.check_sum = 0;
     ack.packet_length = numberOfPackets;
     ack.ack_no = 0;
     char* buf = new char[MSS];
     memset(buf, 0, MSS);
     memcpy(buf, &ack, sizeof(ack));
     ssize_t bytesSent = sendto(client_fd, buf, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
     if (bytesSent == -1) {
        perror("Error Sending The Ack ! ");
        exit(1);
     } else {
        cout << "Ack of file name is sent successfully" << endl;
     }

     /** read data chunks in vector string from file **/
     vector<string> DataPackets = readDataFromFile(fileName);
     if (DataPackets.size() == numberOfPackets){
        cout << "File Data is read successfully " << endl;
     }

     /** start sending data and handling congestion control using the SM **/
    sendTheData_HandleCongestion(client_fd, client_addr, DataPackets);

}

void sendTheData_HandleCongestion (int client_fd, struct sockaddr_in client_addr , vector<string> data){
    // send file in chunks
    // initial window size = 1
    int cwnd_base = 0;
    double cwnd = 1;
    int base_packet_number = 0;
    long sentBytes = 0;
    int ssthresh = 128;
    bool flag = true;
    int seqNum = 0;
    long sentPacketsNotAcked = 0;
    fsm_state state = slow_start;
    long dupAckCounter = 0;
    int expectedAck_no = -1;
    bool stillExistAcks = true;
    char rec_buf[MSS];
    socklen_t client_addr_len = sizeof(struct sockaddr);
    int totalPackets = data.size();
    int alreadySentPackets = 0;

    while (flag){
        /**
        this part will run first to send first datagram.
        **/
        while(cwnd_base < cwnd && alreadySentPackets + lost_packets.size() < totalPackets){
            seqNum = base_packet_number + cwnd_base;
            string temp_packet_string = data[seqNum];
            /**
                in case error simulated won't send the data_packet so the seq_number will not correct at the receiver so will send duplicate ack.
            **/
            bool isSent = send_packet(client_fd, client_addr, temp_packet_string,seqNum);
            if (!isSent) {
                perror("Error sending data data_packet ! ");
                //exit(1);
            } else {
                sentPacketsNotAcked++;
                alreadySentPackets++;
                cout << "Sent Seq Num : " << seqNum << endl;
            }
            // increment base to get next packet in window
            cwnd_base++;
        }

        /*** receiving ACKs for non ack sent packets ***/
        if (sentPacketsNotAcked > 0){
            stillExistAcks = true;
            while (stillExistAcks){
                cout << "waiting ack " << endl;
                ssize_t Received_bytes = recvfrom(client_fd, rec_buf, ACK_PACKET_SIZE, 0, (struct sockaddr*)&client_addr, &client_addr_len);
                if (Received_bytes < 0){
                     perror("error receiving bytes ! ");
                     exit(1);
                }
                else if (Received_bytes != ACK_PACKET_SIZE){
                     cout << "Expecting Ack Got Something Else" << endl;
                     exit(1);
                }
                else {
                    auto ack = (ack_packet*) malloc(sizeof(ack_packet));
                    memcpy(ack, rec_buf, ACK_PACKET_SIZE);
                    cout << "Ack. " << ack->ack_no << " Received." << endl;
                    // bonus
                    if (get_ack_checksum(ack->packet_length, ack->ack_no) != ack->check_sum){
                        cout << "Corrupt Ack. received" << endl;
                    }
                    int ack_no = ack->ack_no;
                    if (expectedAck_no == ack_no){
                        // if expected ack received
                        dupAckCounter++;
                        sentPacketsNotAcked--;
                        if (state == fast_recovery){
                            cwnd++;
                        }
                        else if (dupAckCounter == 3){
                            ssthresh = cwnd / 2;
                            cwnd = ssthresh + 3;
                            state = fast_recovery;
                            /** retransmit the lost data_packets **/
                            seqNum = ack_no;
                            bool found = false;
                            for (int j = 0; j < lost_packets.size() ; j++){
                                // re-send specific lost packet with ack_no
                                lost_packet lostPacket = lost_packets[j];
                                if (lostPacket.seq_no == seqNum){
                                      found = true;
                                      string temp_packet_string = data[seqNum];
                                      struct data_packet data_packet = create_data_packet(temp_packet_string, seqNum);
                                      char sendBuffer [MSS];
                                      memset(sendBuffer, 0, MSS);
                                      memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                                      ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                                      if (bytesSent == -1) {
                                          perror("error re-sending data data_packet ! ");
                                          exit(1);
                                      } else {
                                          sentPacketsNotAcked++;
                                          alreadySentPackets++;
                                          lost_packets.erase(lost_packets.begin() + j);
                                      }
                                      break;
                                }
                            }
                            /** handle checksum error bonus (check sum corrupted error in ack_no value, lost packet is sent but not received by client) **/
                            if (!found){
                                for (int j = 0; j < sent_packets.size() ;j++){
                                    data_packet sentPacket = sent_packets[j];
                                    if (sentPacket.seq_no == seqNum){
                                          found = true;
                                          string temp_packet_string = data[seqNum];
                                          struct data_packet data_packet = create_data_packet(temp_packet_string,seqNum);
                                          char sendBuffer [MSS];
                                          memset(sendBuffer, 0, MSS);
                                          memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                                          ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                                          if (bytesSent == -1) {
                                              perror("error re-sending data data_packet ! ");
                                              exit(1);
                                          } else {
                                              alreadySentPackets++;
                                              sent_packets.erase(sent_packets.begin() + j);
                                          }
                                          break;
                                    }
                                }

                            }

                        }
                    }
                    else if (expectedAck_no < ack_no) {
                        /** new ack : compute new base and data_packet no. and handling congestion control FSM **/
                        cout << "newAck " << endl;
                        dupAckCounter = 0;
                        expectedAck_no = ack_no;
                        cwnd_base = cwnd_base - (expectedAck_no - base_packet_number);
                        base_packet_number = expectedAck_no;
                        if (state == slow_start){
                           cwnd++;
                           if (cwnd >= ssthresh){
                               state = congestion_avoidance;
                           }
                        }
                        else if (state == congestion_avoidance){
                            cwnd += (1 / floor(cwnd));
                        }
                        else if (state == fast_recovery){
                            state = congestion_avoidance;
                            cwnd = ssthresh;
                        }
                        cout << "CWND : " << cwnd << " MSS "<< endl;
                        cout << "sstreash : " << ssthresh << " MSS "<< endl;
                        cout << "CWND_base : " << cwnd_base << " MSS "<< endl;
                        sentPacketsNotAcked--;
                    }
                    else {
                         sentPacketsNotAcked--;
                    }
                    if (sentPacketsNotAcked == 0){
                        stillExistAcks = false;
                    }
                }
            }
        }

        /** Handle Time Out **/
        for (int j = 0; j < lost_packets.size() ; j++){
            lost_packet lostPacket = lost_packets[j];
            chrono::time_point<chrono::system_clock> current_time = chrono::system_clock::now();
            chrono::duration<double> elapsed_time = current_time - lostPacket.packet_timer;
            if (elapsed_time.count() >= 2){
                 cout << "Timed Out ! " << endl;
                 cout << "Re-transmitting the data_packet " << endl;
                 seqNum = lostPacket.seq_no;
                 string temp_packet_string = data[seqNum];
                 struct data_packet data_packet = create_data_packet(temp_packet_string, seqNum);
                 char sendBuffer [MSS];
                 memset(sendBuffer, 0, MSS);
                 memcpy(sendBuffer, &data_packet, sizeof(data_packet));
                 ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
                 if (bytesSent == -1) {
                        perror("error resending the data data_packet ! ");
                        exit(1);
                 } else {
                        sentPacketsNotAcked++;
                        alreadySentPackets++;
                        lost_packets.erase(lost_packets.begin() + j);
                        j--;
                        cout << "Sent Seq Num : " << seqNum << endl;
                 }
            }
        }
    }
}

bool send_packet(int client_fd, struct sockaddr_in client_addr , const string& data, int seq_no){
     char sendBuffer [MSS];
     struct data_packet data_packet = create_data_packet(data, seq_no);
     memcpy(sendBuffer, &data_packet, sizeof(data_packet));
     if (!DropTheDatagram()){
         // packet not lost, will be sent
         ssize_t bytesSent = sendto(client_fd, sendBuffer, MSS, 0, (struct sockaddr *)&client_addr, sizeof(struct sockaddr));
         if (bytesSent == -1) {
                return false;
         }
         else {
                sent_packets.push_back(data_packet);
                return true;
         }
     }
     else {
         // packet loss happen
         struct lost_packet lostPacket{};
         lostPacket.seq_no = seq_no;
         lostPacket.packet_timer = chrono::system_clock::now();
         lost_packets.push_back(lostPacket);

        return false;
     }
}

data_packet create_data_packet(const string& data, int seq_no) {
    struct data_packet p{};
    strcpy(p.data, data.c_str());
    p.seq_no = seq_no;
    p.packet_length = data.size();
    p.check_sum = get_data_checksum(data, p.packet_length, p.seq_no);
    return p;

}

long checkFileExistence(const string& fileName){
     ifstream file(fileName.c_str(), ifstream::ate | ifstream::binary);
     if (!file.is_open()) {
        cout << "Error Open the requested file" << endl;
        return -1;
     }
     cout << "the File is opened successfully" << endl;
     long len = file.tellg();
     file.close();
     return len;
}

vector<string> readDataFromFile(const string& fileName){
    vector<string> DataPackets;
    string temp;
    ifstream fin;
    fin.open(fileName);
    if (fin){
        char c;
        int char_counter = 0;
        while(fin.get(c)){
            if(char_counter < CHUNK_SIZE) {
                temp += c;
            }else{
                DataPackets.push_back(temp);
                temp.clear();
                temp += c;
                char_counter = 0;
                continue;
            }
            char_counter++;
        }
        if (char_counter > 0){
            DataPackets.push_back(temp);
        }
    }
    fin.close();
    return DataPackets;
}

vector<string> readArgsFile(){
    string fileName = "info.txt";
    vector<string> commands;
    string command;
    string content;
    ifstream file;
    file.open(fileName);
    while(getline(file, command)){
        commands.push_back(command);
    }
    return commands;
}

bool DropTheDatagram(){
    double packet_loss_prob = (rand() % 100) * PLP;
    cout << "packet_loss_prob: " << packet_loss_prob << endl;
    if (packet_loss_prob >= 5.9){
        return true;
    }
    return false;
}

uint16_t get_ack_checksum (uint16_t packet_length , uint32_t ack_no){
    uint32_t sum = 0;
    sum = packet_length + ack_no;
    while (sum >> 16){
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    auto complement_sum = (uint16_t) (~sum);
    return complement_sum;
}

uint16_t get_data_checksum (const string& content, uint16_t len , uint32_t seq_no){
    uint32_t sum = 0;
    sum = len + seq_no;
    char arr[content.length()+1];
    strcpy(arr, content.c_str());
    for (int i = 0; i < content.length(); i++){
        sum += arr[i];
    }
    while (sum >> 16){
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    auto complement_sum = (uint16_t) (~sum);
    return complement_sum;
}