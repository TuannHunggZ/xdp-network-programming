#include <iostream>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <iomanip>
#include <vector>
#include <map>
#include <algorithm>

#define CHUNK_SIZE 972
#define HEADER_SIZE 4
#define ACK_TIMEOUT_MS 500
#define DEFAULT_WINDOW_SIZE 5
#define MAX_WINDOW_SIZE 8191
#define HANDSHAKE_TIMEOUT_MS 2000
#define MAX_HANDSHAKE_RETRIES 5

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

struct WindowPacket {
    std::vector<char> data;
    uint32_t pkt_num;
    std::chrono::high_resolution_clock::time_point send_time;
    bool acked;
    int retry_count;
};

bool performHandshake(int sock, struct sockaddr_in& receiver_addr, uint16_t proposed_window, uint16_t& negotiated_window) {
    std::cout << "\n=== BẮT ĐẦU HANDSHAKE ===" << std::endl;
    std::cout << "Window size đề xuất: " << proposed_window << std::endl;
    
    HandshakePacket syn_packet;
    HandshakePacket response;
    struct sockaddr_in response_addr;
    socklen_t addr_len = sizeof(response_addr);
    
    // Bước 1: Gửi SYN với window size đề xuất
    for (int retry = 0; retry < MAX_HANDSHAKE_RETRIES; retry++) {
        syn_packet.setWindowSize(proposed_window);
        syn_packet.setFlags(SYN);
        
        std::cout << "Bước 1: Gửi SYN với window_size=" << syn_packet.getWindowSize() 
                  << " đến receiver..." << std::endl;
        
        ssize_t sent = sendto(sock, &syn_packet, sizeof(syn_packet), 0,
                             (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
        
        if (sent < 0) {
            std::cerr << "Lỗi khi gửi SYN" << std::endl;
            continue;
        }
        
        // Đợi SYN-ACK với timeout
        auto start_time = std::chrono::high_resolution_clock::now();
        while (true) {
            ssize_t recv_len = recvfrom(sock, &response, sizeof(response), 0,
                                       (struct sockaddr*)&response_addr, &addr_len);
            
            if (recv_len > 0 && (response.getFlags() & (SYN | ACK)) == (SYN | ACK)) {
                negotiated_window = response.getWindowSize();
                std::cout << "Bước 2: Nhận được SYN-ACK từ receiver" << std::endl;
                std::cout << "        Window size được thỏa thuận: " << negotiated_window << std::endl;
                
                // Bước 3: Gửi ACK với window size đã thỏa thuận
                HandshakePacket ack_packet;
                ack_packet.setWindowSize(negotiated_window);
                ack_packet.setFlags(ACK);
                
                std::cout << "Bước 3: Gửi ACK để hoàn tất handshake" << std::endl;
                sendto(sock, &ack_packet, sizeof(ack_packet), 0,
                      (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
                
                std::cout << "✓ Handshake thành công!" << std::endl;
                std::cout << "✓ Window size cuối cùng: " << negotiated_window << std::endl;
                std::cout << "=== KẾT THÚC HANDSHAKE ===\n" << std::endl;
                return true;
            }
            
            // Check timeout
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
            if (elapsed.count() >= HANDSHAKE_TIMEOUT_MS) {
                std::cout << "Timeout! Thử lại lần " << (retry + 1) << "/" << MAX_HANDSHAKE_RETRIES << std::endl;
                break;
            }
        }
    }
    
    std::cerr << "✗ Handshake thất bại sau " << MAX_HANDSHAKE_RETRIES << " lần thử!" << std::endl;
    return false;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <file_path> <receiver_ip> <port>" << std::endl;
        return 1;
    }

    const char* file_path = argv[1];
    const char* receiver_ip = argv[2];
    int port = std::stoi(argv[3]);
    uint16_t proposed_window = DEFAULT_WINDOW_SIZE;

    std::cout << "Sử dụng giao thức: Selective Repeat với Handshake (16-bit)" << std::endl;

    // Mở file
    std::ifstream file(file_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Không thể mở file: " << file_path << std::endl;
        return 1;
    }

    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::cout << "Kích thước file: " << file_size << " bytes (" 
              << std::fixed << std::setprecision(2) << file_size / 1024.0 / 1024.0 << " MB)" << std::endl;

    // ĐỌC TOÀN BỘ FILE VÀO MEMORY
    std::cout << "Đang đọc file vào memory..." << std::endl;
    std::vector<char> file_data(file_size);
    file.read(file_data.data(), file_size);
    file.close();
    std::cout << "Đã đọc xong file vào memory!" << std::endl;

    // Tính số packet
    uint64_t total_packets = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    std::cout << "Tổng số packets: " << total_packets << std::endl;

    // Tạo UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Không thể tạo socket" << std::endl;
        return 1;
    }

    // Set socket timeout cho handshake
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Cấu hình địa chỉ receiver
    struct sockaddr_in receiver_addr;
    memset(&receiver_addr, 0, sizeof(receiver_addr));
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_port = htons(port);
    inet_pton(AF_INET, receiver_ip, &receiver_addr.sin_addr);

    // Thực hiện handshake và thỏa thuận window size
    uint16_t negotiated_window;
    if (!performHandshake(sock, receiver_addr, proposed_window, negotiated_window)) {
        std::cerr << "Không thể kết nối đến receiver!" << std::endl;
        close(sock);
        return 1;
    }

    std::cout << "Sử dụng window size: " << negotiated_window << std::endl;

    // Đổi timeout cho data transfer
    tv.tv_usec = 10000; // 10ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Bắt đầu đo thời gian (SAU khi handshake hoàn tất)
    auto start_time = std::chrono::high_resolution_clock::now();
    auto last_progress_time = start_time;

    // Sliding window với negotiated window size
    std::map<uint32_t, WindowPacket> window;
    uint32_t base = 1;
    uint32_t next_seq_num = 1;
    
    uint64_t total_bytes_sent = 0;
    uint64_t total_retransmissions = 0;
    uint64_t acks_received = 0;

    std::cout << "Bắt đầu truyền dữ liệu từ memory với Selective Repeat..." << std::endl;

    while (base <= total_packets) {
        auto now = std::chrono::high_resolution_clock::now();

        // Gửi các packet mới trong window
        while (next_seq_num < base + negotiated_window && next_seq_num <= total_packets) {
            WindowPacket pkt;
            pkt.pkt_num = next_seq_num;
            pkt.acked = false;
            pkt.retry_count = 0;

            size_t offset = (next_seq_num - 1) * CHUNK_SIZE;
            size_t chunk_size = std::min((size_t)CHUNK_SIZE, file_data.size() - offset);

            pkt.data.resize(HEADER_SIZE + chunk_size);
            PacketHeader* header = (PacketHeader*)pkt.data.data();
            header->pkt_num = next_seq_num;
            memcpy(pkt.data.data() + HEADER_SIZE, file_data.data() + offset, chunk_size);

            pkt.send_time = now;
            ssize_t sent = sendto(sock, pkt.data.data(), pkt.data.size(), 0,
                                 (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));

            if (sent > 0) {
                window[next_seq_num] = pkt;
                total_bytes_sent += (sent - HEADER_SIZE);
            }
            
            next_seq_num++;
        }

        // Kiểm tra timeout và gửi lại
        for (auto& pair : window) {
            uint32_t seq = pair.first;
            WindowPacket& pkt = pair.second;
            
            if (!pkt.acked) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - pkt.send_time);
                
                if (elapsed.count() >= ACK_TIMEOUT_MS) {
                    pkt.send_time = now;
                    pkt.retry_count++;
                    
                    sendto(sock, pkt.data.data(), pkt.data.size(), 0,
                          (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
                    total_retransmissions++;
                }
            }
        }

        // Nhận ACK
        AckPacket ack;
        struct sockaddr_in ack_addr;
        socklen_t ack_addr_len = sizeof(ack_addr);
        
        ssize_t ack_recv = recvfrom(sock, &ack, sizeof(ack), 0,
                                   (struct sockaddr*)&ack_addr, &ack_addr_len);

        if (ack_recv > 0) {
            uint32_t ack_num = ack.ack_num;
            acks_received++;

            if (window.find(ack_num) != window.end()) {
                window[ack_num].acked = true;
            }
            
            while (window.find(base) != window.end() && window[base].acked) {
                window.erase(base);
                base++;
            }
        }

        // Hiển thị tiến trình
        auto progress_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time);
        if (progress_elapsed.count() >= 500) {
            float progress = (float)(base - 1) / total_packets * 100;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
            double speed = (total_bytes_sent / 1024.0 / 1024.0) / (elapsed.count() / 1000.0);
            
            std::cout << "\rTiến trình: " << (base - 1) << "/" << total_packets 
                     << " (" << std::fixed << std::setprecision(1) << progress << "%) - "
                     << std::setprecision(2) << speed << " MB/s - "
                     << "Window: [" << base << "-" << (next_seq_num - 1) << "] - "
                     << "Retrans: " << total_retransmissions << std::flush;
            
            last_progress_time = now;
        }
    }

    // Kết thúc
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "\n\n=== KẾT QUẢ GỬI (Selective Repeat) ===" << std::endl;
    std::cout << "Window size đã sử dụng: " << negotiated_window << std::endl;
    std::cout << "Tổng thời gian: " << std::fixed << std::setprecision(3) 
              << duration.count() / 1000.0 << " giây" << std::endl;
    std::cout << "Tổng số packets: " << total_packets << std::endl;
    std::cout << "ACKs nhận được: " << acks_received << std::endl;
    std::cout << "Tổng số lần truyền lại: " << total_retransmissions << std::endl;
    std::cout << "Tỷ lệ truyền lại: " << std::setprecision(2)
              << (total_packets > 0 ? (total_retransmissions * 100.0 / total_packets) : 0) << "%" << std::endl;
    std::cout << "Tổng dữ liệu đã gửi: " << std::setprecision(2) 
              << total_bytes_sent / 1024.0 / 1024.0 << " MB" << std::endl;
    std::cout << "Tốc độ trung bình: " << std::setprecision(2) 
              << (total_bytes_sent / 1024.0 / 1024.0) / (duration.count() / 1000.0) 
              << " MB/s" << std::endl;
    std::cout << "Tốc độ trung bình: " << std::setprecision(2) 
              << (total_bytes_sent * 8.0 / 1024.0 / 1024.0) / (duration.count() / 1000.0) 
              << " Mbps" << std::endl;

    close(sock);
    return 0;
}