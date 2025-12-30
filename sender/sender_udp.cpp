#include <iostream>
#include <fstream>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <chrono>
#include <iomanip>

#define CHUNK_SIZE 1024

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <file_path> <receiver_ip> <port>" << std::endl;
        return 1;
    }

    const char* file_path = argv[1];
    const char* receiver_ip = argv[2];
    int port = std::stoi(argv[3]);

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

    // Cấu hình địa chỉ receiver
    struct sockaddr_in receiver_addr;
    memset(&receiver_addr, 0, sizeof(receiver_addr));
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_port = htons(port);
    inet_pton(AF_INET, receiver_ip, &receiver_addr.sin_addr);

    std::cout << "Đang kết nối đến " << receiver_ip << ":" << port << "..." << std::endl;

    // Bắt đầu đo thời gian
    auto start_time = std::chrono::high_resolution_clock::now();

    uint64_t packets_sent = 0;
    uint64_t total_bytes_sent = 0;
    size_t offset = 0;

    std::cout << "Bắt đầu gửi dữ liệu từ memory (UDP)..." << std::endl;

    // Gửi dữ liệu từ memory
    while (offset < file_size) {
        size_t chunk_size = std::min((size_t)CHUNK_SIZE, file_size - offset);

        // Gửi packet trực tiếp từ memory
        ssize_t sent = sendto(sock, file_data.data() + offset, chunk_size, 0,
                             (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));

        if (sent < 0) {
            std::cerr << "Lỗi gửi packet " << packets_sent << std::endl;
        } else {
            total_bytes_sent += sent;
        }

        packets_sent++;
        offset += chunk_size;

        // Hiển thị tiến trình
        if (packets_sent % 100 == 0) {
            float progress = (float)packets_sent / total_packets * 100;
            auto current_time = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - start_time);
            double speed = (total_bytes_sent / 1024.0 / 1024.0) / (elapsed.count() / 1000.0);
            
            std::cout << "\rĐã gửi: " << packets_sent << "/" << total_packets 
                     << " (" << std::fixed << std::setprecision(1) << progress << "%) - "
                     << std::setprecision(2) << speed << " MB/s" << std::flush;
        }
    }

    // Kết thúc đo thời gian
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    std::cout << "\n\n=== KẾT QUẢ GỬI (UDP) ===" << std::endl;
    std::cout << "Tổng thời gian: " << std::fixed << std::setprecision(3) 
              << duration.count() / 1000.0 << " giây" << std::endl;
    std::cout << "Tổng số packets đã gửi: " << packets_sent << std::endl;
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