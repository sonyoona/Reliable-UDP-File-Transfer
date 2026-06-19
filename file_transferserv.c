#include "file_transfer.h"
#include <time.h>

#define LOSS_RATE 0  // [성능 실험용] % 패킷 유실률 의미

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_sz;

    if (argc != 2) {
        printf("사용법 : %s <Port>\n", argv[0]);
        exit(1);
    }

    // 난수 생성기 초기화 (유실 확률 계산용)
    srand(time(NULL));

    sock = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(atoi(argv[1]));

    if (bind(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind() 실패");
        exit(1);
    }

    printf("=============================================\n");
    printf(" GBN 수신 서버 가동 시작 (설정된 유실률: %d%%)\n", LOSS_RATE);
    printf("=============================================\n");

    int expected_seq = 0; // 서버가 기다리는 다음 패킷 번호
    FILE *fp = NULL;

    while (1) {
        Packet packet;
        client_addr_sz = sizeof(client_addr);
        int recv_len = recvfrom(sock, &packet, sizeof(packet), 0, (struct sockaddr*)&client_addr, &client_addr_sz);

		//수신한 데이터가 최소 헤더 크기(type, seq, size)를 만족하는가 확인 
        if (recv_len < HEADER_SIZE) continue;

        int p_type = ntohl(packet.type);
        int p_seq = ntohl(packet.seq);
        int p_size = ntohl(packet.size);

        // 1. DATA 패킷 수신 처리
        if (p_type == TYPE_DATA) {
            //  [핵심 성능 측정 장치] 확률적 패킷 유실 시뮬레이션
            if (rand() % 100 < LOSS_RATE) {
                printf("[LOSS ] 의도적 패킷 유실 작동 ➔ SEQ:%d 드롭함\n", p_seq);
                continue; // 아래 코드를 실행하지 않고 완전히 무시 (클라이언트 타임아웃 유도)
            }

            // 첫 패킷을 받으면 저장할 파일 열기
            if (fp == NULL) {
                fp = fopen("received_backend.bin", "wb");
                if (!fp) { perror("파일 오픈 실패"); exit(1); }
            }

            // GBN의 핵심 규칙: 순서가 딱 맞아떨어지는 패킷만 승인한다!
            if (p_seq == expected_seq) {
                printf("[RCV DATA] 순서 일치 수신 성공 ➔ SEQ:%d (크기: %d 바이트)\n", p_seq, p_size);
                
                // 파일에 데이터 기록
                fwrite(packet.data, 1, p_size, fp);
                
                // 성공했으므로 ACK 전송 및 다음 기다릴 번호 증가
                AckPacket ack;
                ack.type = htonl(TYPE_ACK);
                ack.seq = htonl(p_seq);
                sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&client_addr, client_addr_sz);
                
                expected_seq++;
            } else {
                // 순서가 어긋난 패킷이 오면 버리고, "마지막으로 성공했던 ACK"를 재전송해 누적 ACK 유도
                printf("[RCV DUP] 순서 불일치 패킷 무시 ➔ 수신 SEQ:%d (기다리던 SEQ:%d) ➔ ACK:%d 재전송\n", 
                       p_seq, expected_seq, expected_seq - 1);
                
                if (expected_seq > 0) { //expected_seq가 -1이 되는것을 방지 
                    AckPacket ack;
                    ack.type = htonl(TYPE_ACK);
                    ack.seq = htonl(expected_seq - 1);
                    sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&client_addr, client_addr_sz);
                }
            }
        }
        // 2. FIN (종료) 패킷 수신 처리
        else if (p_type == TYPE_FIN) {
            printf("\n=============================================\n");
            printf("[RCV FIN] 클라이언트로부터 전송 종료 요청(FIN) 수신\n");
            
            // FIN에 대한 응답 ACK 전송
            AckPacket ack;
            ack.type = htonl(TYPE_ACK);
            ack.seq = htonl(p_seq); // 마지막 base 수치가 담겨 옴
            sendto(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&client_addr, client_addr_sz);
            printf("[SEND FIN_ACK] 최종 종료 확인 패킷 회신 완료\n");
            
            if (fp) {
                fclose(fp);
                fp = NULL;
            }
            printf(" 파일 저장 완료! 다음 전송을 위해 대기 상태로 리셋합니다.\n");
            printf("=============================================\n\n");
            
            expected_seq = 0; // 다음 테스트를 위해 시퀀스 초기화
        }
    }

    close(sock);
    return 0;
}
