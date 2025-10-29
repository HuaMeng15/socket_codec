// #include <stdio.h>
// #include <string.h>
// #include <sys/socket.h>
// #include <netinet/in.h>  // 包含sockaddr_in结构体
// #include <arpa/inet.h>   // 包含inet_addr等函数
// #include <unistd.h>

// int main() {
//     // 1. 创建UDP套接字
//     int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
//     if (sockfd < 0) {
//         perror("socket creation failed");
//         return 1;
//     }

//     // 2. 设置目标地址信息
//     struct sockaddr_in dest_addr;
//     memset(&dest_addr, 0, sizeof(dest_addr));  // 初始化结构体
//     dest_addr.sin_family = AF_INET;
//     dest_addr.sin_port = htons(8888);  // 目标端口（网络字节序）
//     // 转换目标IP为网络字节序（替换为实际IP）
//     if (inet_pton(AF_INET, "127.0.0.1", &dest_addr.sin_addr) <= 0) {
//         perror("invalid address/address not supported");
//         close(sockfd);
//         return 1;
//     }

//     // 3. 待发送的消息
//     const char* message = "Hello, UDP receiver!";
//     int msg_len = strlen(message);

//     // 4. 发送消息
//     int bytes_sent = sendto(
//         sockfd,
//         message,
//         msg_len,
//         0,
//         (struct sockaddr*)&dest_addr,
//         sizeof(dest_addr)
//     );

//     if (bytes_sent < 0) {
//         perror("sendto failed");
//         close(sockfd);
//         return 1;
//     }

//     printf("Sent %d bytes to %s:%d\n", 
//            bytes_sent, 
//            inet_ntoa(dest_addr.sin_addr), 
//            ntohs(dest_addr.sin_port)
//           );

//     // 5. 关闭套接字
//     close(sockfd);
//     return 0;
// }