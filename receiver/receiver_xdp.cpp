#include <iostream>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <map>
#include <vector>

#define CHUNK_SIZE 972
#define TIMEOUT_SEC 5
#define HEADER_SIZE 4
#define DEFAULT_WINDOW_SIZE 5
#define MAX_WINDOW_SIZE 8191  // 2^13 - 1 (13 bits)

// Handshake flags (3 bits cuối)
#define SYN 0x01   // 0000 0001 - Yêu cầu kết nối
#define ACK 0x02   // 0000 0010 - Xác nhận
#define FIN 0x04   // 0000 0100 - Kết thúc kết nối

// Handshake packet structure (16 bits = 2 bytes)
// Format: [13 bits: window_size][3 bits: flags]
struct HandshakePacket {
    uint16_t data;  // 13 bits window size + 3 bits flags
    
    // Set window size (13 bits đầu)
    void setWindowSize(uint16_t window_size) {
        if (window_size > MAX_WINDOW_SIZE) {
            window_size = MAX_WINDOW_SIZE;
        }
        data = (window_size << 3) | (data & 0x07);
    }
    
    // Get window size
    uint16_t getWindowSize() const {
        return (data >> 3) & 0x1FFF;  // Lấy 13 bits đầu
    }
    
    // Set flags (3 bits cuối)
    void setFlags(uint8_t flags) {
        data = (data & 0xFFF8) | (flags & 0x07);
    }
    
    // Get flags
    uint8_t getFlags() const {
        return data & 0x07;  // Lấy 3 bits cuối
    }
};

struct PacketHeader {
    uint32_t pkt_num;
};

struct AckPacket {
    uint32_t ack_num;
};

struct BufferedPacket {
    std::vector<char> data;
    bool received;
};

bool waitForHandshake(int sock, struct sockaddr_in& sender_addr, socklen_t& addr_len, 
                     uint16_t preferred_window, uint16_t& negotiated_window) {
    std::cout << "\n=== CHỜ HANDSHAKE ===" << std::endl;
    std::cout << "Đang đợi yêu cầu kết nối từ sender..." << std::endl;
    std::cout << "Window size ưa thích của receiver: " << preferred_window << std::endl;
    
    HandshakePacket packet;
    
    while (true) {
        ssize_t recv_len = recvfrom(sock, &packet, sizeof(packet), 0,
                                    (struct sockaddr*)&sender_addr, &addr_len);
        
        if (recv_len < 0) {
            continue;
        }
        
        // Kiểm tra nếu là gói tin handshake (kích thước đúng)
        if (recv_len == sizeof(HandshakePacket)) {
            // Bước 1: Nhận SYN
            if (packet.getFlags() & SYN) {
                uint16_t sender_window = packet.getWindowSize();
                
                char sender_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);
                std::cout << "Bước 1: Nhận được SYN từ " << sender_ip << ":" << ntohs(sender_addr.sin_port) << std::endl;
                std::cout << "        Sender đề xuất window_size=" << sender_window << std::endl;
                
                // Thỏa thuận window size (chọn giá trị nhỏ hơn)
                negotiated_window = std::min(sender_window, preferred_window);
                std::cout << "        Receiver chọn window_size=" << negotiated_window << std::endl;
                
                // Bước 2: Gửi SYN-ACK với window size đã chọn
                HandshakePacket syn_ack;
                syn_ack.setWindowSize(negotiated_window);
                syn_ack.setFlags(SYN | ACK);
                
                std::cout << "Bước 2: Gửi SYN-ACK với window_size=" << negotiated_window << std::endl;
                sendto(sock, &syn_ack, sizeof(syn_ack), 0,
                      (struct sockaddr*)&sender_addr, addr_len);
                
                // Bước 3: Đợi ACK
                auto start_time = std::chrono::high_resolution_clock::now();
                while (true) {
                    HandshakePacket ack_packet;
                    struct sockaddr_in temp_addr;
                    socklen_t temp_len = sizeof(temp_addr);
                    
                    ssize_t ack_len = recvfrom(sock, &ack_packet, sizeof(ack_packet), 0,
                                              (struct sockaddr*)&temp_addr, &temp_len);
                    
                    if (ack_len > 0 && ack_len == sizeof(HandshakePacket) && (ack_packet.getFlags() & ACK)) {
                        std::cout << "Bước 3: Nhận được ACK" << std::endl;
                        std::cout << "✓ Handshake thành công!" << std::endl;
                        std::cout << "✓ Window size cuối cùng: " << negotiated_window << std::endl;
                        std::cout << "=== KẾT THÚC HANDSHAKE ===\n" << std::endl;
                        return true;
                    }
                    
                    // Timeout - gửi lại SYN-ACK
                    auto now = std::chrono::high_resolution_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
                    if (elapsed.count() >= 1000) {
                        std::cout << "Timeout! Gửi lại SYN-ACK..." << std::endl;
                        sendto(sock, &syn_ack, sizeof(syn_ack), 0,
                              (struct sockaddr*)&sender_addr, addr_len);
                        start_time = now;
                    }
                }
            }
        }
    }
    
    return false;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <port> <output_file> <original_file>" << std::endl;
        return 1;
    }

    int port = std::stoi(argv[1]);
    const char* output_file = argv[2];
    const char* original_file = argv[3];
    uint16_t preferred_window = DEFAULT_WINDOW_SIZE;
    
    std::cout << "Sử dụng giao thức: Selective Repeat với Handshake (16-bit)" << std::endl;

    // Kiểm tra file gốc
    std::ifstream orig_file(original_file, std::ios::binary | std::ios::ate);
    if (!orig_file.is_open()) {
        std::cerr << "Không thể mở file gốc: " << original_file << std::endl;
        return 1;
    }
    
    std::streamsize original_size = orig_file.tellg();
    orig_file.close();
    
    std::cout << "Kích thước file gốc: " << std::fixed << std::setprecision(2) 
              << original_size / 1024.0 / 1024.0 << " MB" << std::endl;

    // CẤP PHÁT MEMORY ĐỂ LƯU DỮ LIỆU
    std::cout << "Cấp phát memory để nhận dữ liệu..." << std::endl;
    std::vector<char> received_data;
    received_data.reserve(original_size);
    std::cout << "Đã cấp phát " << std::setprecision(2) 
              << original_size / 1024.0 / 1024.0 << " MB memory!" << std::endl;

    // Tạo UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Không thể tạo socket" << std::endl;
        return 1;
    }

    // Set timeout
    struct timeval tv;
    tv.tv_sec = TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cerr << "Không thể bind socket" << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Đang lắng nghe trên port " << port << "..." << std::endl;

    // Chờ handshake và thỏa thuận window size
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    uint16_t negotiated_window;
    
    if (!waitForHandshake(sock, sender_addr, addr_len, preferred_window, negotiated_window)) {
        std::cerr << "Handshake thất bại!" << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Sử dụng window size: " << negotiated_window << std::endl;

    char buffer[CHUNK_SIZE + HEADER_SIZE];

    uint32_t expected_seq_num = 1;
    std::map<uint32_t, BufferedPacket> receive_buffer;
    
    uint64_t packets_received = 0;
    uint64_t total_bytes_received = 0;
    uint64_t duplicate_packets = 0;
    uint64_t out_of_order_packets = 0;
    uint64_t acks_sent = 0;

    auto start_time = std::chrono::high_resolution_clock::now();
    auto last_packet_time = start_time;
    auto last_progress_time = start_time;

    std::cout << "Đang nhận dữ liệu vào memory với Selective Repeat..." << std::endl;

    while (true) {
        ssize_t recv_len = recvfrom(sock, buffer, sizeof(buffer), 0,
                                    (struct sockaddr*)&sender_addr, &addr_len);

        if (recv_len < 0) {
            auto now = std::chrono::high_resolution_clock::now();
            auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(now - last_packet_time);
            
            if (idle_time.count() >= TIMEOUT_SEC) {
                std::cout << "\nTimeout - kết thúc nhận dữ liệu" << std::endl;
                break;
            }
            continue;
        }

        // Bỏ qua gói tin handshake nếu nhận được
        if (recv_len == sizeof(HandshakePacket)) {
            continue;
        }

        last_packet_time = std::chrono::high_resolution_clock::now();

        // Parse header
        PacketHeader* header = (PacketHeader*)buffer;
        uint32_t pkt_num = header->pkt_num;

        // Selective Repeat logic với negotiated window size
        if (pkt_num >= expected_seq_num && pkt_num < expected_seq_num + negotiated_window) {
            // Gửi ACK
            AckPacket ack;
            ack.ack_num = pkt_num;
            sendto(sock, &ack, sizeof(ack), 0,
                  (struct sockaddr*)&sender_addr, addr_len);
            acks_sent++;

            if (pkt_num == expected_seq_num) {
                // Packet đúng thứ tự - LƯU VÀO MEMORY
                size_t data_size = recv_len - HEADER_SIZE;
                received_data.insert(received_data.end(), 
                                    buffer + HEADER_SIZE, 
                                    buffer + HEADER_SIZE + data_size);
                packets_received++;
                total_bytes_received += data_size;
                expected_seq_num++;

                // Kiểm tra buffer
                while (receive_buffer.find(expected_seq_num) != receive_buffer.end()) {
                    BufferedPacket& buffered = receive_buffer[expected_seq_num];
                    received_data.insert(received_data.end(), 
                                        buffered.data.begin(), 
                                        buffered.data.end());
                    packets_received++;
                    total_bytes_received += buffered.data.size();
                    receive_buffer.erase(expected_seq_num);
                    expected_seq_num++;
                }

            } else if (pkt_num > expected_seq_num) {
                // Packet đến sớm - buffer
                if (receive_buffer.find(pkt_num) == receive_buffer.end()) {
                    BufferedPacket buffered;
                    buffered.data.resize(recv_len - HEADER_SIZE);
                    memcpy(buffered.data.data(), buffer + HEADER_SIZE, recv_len - HEADER_SIZE);
                    buffered.received = true;
                    receive_buffer[pkt_num] = buffered;
                    out_of_order_packets++;
                } else {
                    duplicate_packets++;
                }
            } else {
                duplicate_packets++;
            }

        } else if (pkt_num < expected_seq_num) {
            duplicate_packets++;
            
            AckPacket ack;
            ack.ack_num = pkt_num;
            sendto(sock, &ack, sizeof(ack), 0,
                  (struct sockaddr*)&sender_addr, addr_len);
            acks_sent++;
        }

        // Hiển thị tiến trình
        auto now = std::chrono::high_resolution_clock::now();
        auto progress_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time);
        if (progress_elapsed.count() >= 500) {
            std::cout << "\rĐã nhận: " << packets_received << " packets - "
                     << std::fixed << std::setprecision(2)
                     << total_bytes_received / 1024.0 / 1024.0 << " MB - "
                     << "Buffered: " << receive_buffer.size() << std::flush;
            last_progress_time = now;
        }
    }

    // Kết thúc
    auto end_time = last_packet_time;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "\n\nĐang ghi dữ liệu từ memory ra file..." << std::endl;

    // GHI DỮ LIỆU TỪ MEMORY RA FILE (không tính vào thời gian đo)
    std::ofstream file(output_file, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Không thể tạo file output: " << output_file << std::endl;
        close(sock);
        return 1;
    }
    
    file.write(received_data.data(), received_data.size());
    file.close();
    std::cout << "Đã ghi xong file!" << std::endl;

    // Lấy kích thước file thực tế
    std::ifstream check_file(output_file, std::ios::binary | std::ios::ate);
    std::streamsize received_size = check_file.tellg();
    check_file.close();

    // Tính toán
    int64_t data_lost = original_size - received_size;
    double loss_rate = (original_size > 0) ? (data_lost * 100.0 / original_size) : 0;

    std::cout << "\n=== KẾT QUẢ NHẬN (Selective Repeat) ===" << std::endl;
    std::cout << "Window size đã sử dụng: " << negotiated_window << std::endl;
    std::cout << "Tổng thời gian: " << std::fixed << std::setprecision(3) 
              << duration.count() / 1000.0 << " giây" << std::endl;
    std::cout << "Packets đã nhận: " << packets_received << std::endl;
    std::cout << "ACKs đã gửi: " << acks_sent << std::endl;
    std::cout << "Packets trùng lặp: " << duplicate_packets << std::endl;
    std::cout << "Packets không theo thứ tự: " << out_of_order_packets << std::endl;
    std::cout << "Packets còn trong buffer: " << receive_buffer.size() << std::endl;
    std::cout << "Tổng dữ liệu đã nhận: " << std::setprecision(2) 
              << total_bytes_received / 1024.0 / 1024.0 << " MB" << std::endl;
    std::cout << "File gốc: " << std::setprecision(2) 
              << original_size / 1024.0 / 1024.0 << " MB" << std::endl;
    std::cout << "File nhận được: " << std::setprecision(2) 
              << received_size / 1024.0 / 1024.0 << " MB" << std::endl;
    std::cout << "Dữ liệu bị mất: " << std::setprecision(2) 
              << data_lost / 1024.0 / 1024.0 << " MB" << std::endl;
    std::cout << "Tỷ lệ mất dữ liệu: " << std::setprecision(4) 
              << loss_rate << "%" << std::endl;
    std::cout << "Tốc độ trung bình: " << std::setprecision(2) 
              << (total_bytes_received / 1024.0 / 1024.0) / (duration.count() / 1000.0) 
              << " MB/s" << std::endl;
    std::cout << "Tốc độ trung bình: " << std::setprecision(2) 
              << (total_bytes_received * 8.0 / 1024.0 / 1024.0) / (duration.count() / 1000.0) 
              << " Mbps" << std::endl;

    close(sock);
    return 0;
}