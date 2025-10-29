// #include <stdio.h>
// #include <string.h>
// #include <sys/socket.h>
// #include <netinet/in.h>
// #include <arpa/inet.h>
// #include <unistd.h>

// int main() {
//     // 1. 创建UDP套接字
//     int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
//     if (sockfd < 0) {
//         perror("socket creation failed");
//         return 1;
//     }

//     // 2. 设置本地绑定地址
//     struct sockaddr_in local_addr;
//     memset(&local_addr, 0, sizeof(local_addr));
//     local_addr.sin_family = AF_INET;
//     local_addr.sin_port = htons(8888);  // 绑定到8888端口
//     local_addr.sin_addr.s_addr = INADDR_ANY;  // 监听所有本地IP

//     // 3. 绑定套接字
//     if (bind(sockfd, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
//         perror("bind failed");
//         close(sockfd);
//         return 1;
//     }

//     printf("Waiting for UDP messages on port 4020...\n");

//     // 4. 接收消息
//     char buffer[1024] = {0};
//     struct sockaddr_in sender_addr;
//     socklen_t sender_addr_len = sizeof(sender_addr);

//     int bytes_recv = recvfrom(
//         sockfd,
//         buffer,
//         sizeof(buffer) - 1,
//         0,
//         (struct sockaddr*)&sender_addr,
//         &sender_addr_len
//     );

//     if (bytes_recv < 0) {
//         perror("recvfrom failed");
//         close(sockfd);
//         return 1;
//     }

//     // 5. 打印接收结果
//     buffer[bytes_recv] = '\0';
//     printf("Received %d bytes from %s:%d\n", 
//            bytes_recv, 
//            inet_ntoa(sender_addr.sin_addr), 
//            ntohs(sender_addr.sin_port)
//           );
//     printf("Message: %s\n", buffer);

//     // 6. 关闭套接字
//     close(sockfd);
//     return 0;
// }