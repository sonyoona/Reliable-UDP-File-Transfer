#include "file_transfer.h"

#define WINDOW_SIZE 8
#define TIMEOUT_MS 1000 // 1초(1000ms) 타임아웃

long long get_current_time_ms(void);
int send_fin_with_retry(int sock, int fin_seq, struct sockaddr_in *receiver_addr);

int main(int argc, char *argv[]) {
    int sock;
    struct sockaddr_in receiver_addr;
    FILE *fp;

    if (argc != 4) {
        printf("사용법 : %s <IP> <Port> <파일명>\n", argv[0]);
        exit(1);
    }

    sock = socket(PF_INET, SOCK_DGRAM, 0);
    memset(&receiver_addr, 0, sizeof(receiver_addr));
    receiver_addr.sin_family = AF_INET;
    receiver_addr.sin_addr.s_addr = inet_addr(argv[1]);
    receiver_addr.sin_port = htons(atoi(argv[2]));

    fp = fopen(argv[3], "rb");
    if (!fp) { perror("파일 열기 실패"); exit(1); }

    // 1. 파일 크기 측정 (fseek & ftell 공식 적용)
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // 올림 계산 트릭으로 총 패킷 수 계산
    int total_packets = (file_size + DATA_SIZE - 1) / DATA_SIZE;
    if (total_packets == 0) total_packets = 1;

    Packet *packet_buffer = (Packet *)calloc(total_packets, sizeof(Packet));
    for (int i = 0; i < total_packets; i++) {
        int read_len = fread(packet_buffer[i].data, 1, DATA_SIZE, fp);
		//파일에서 데이터를 읽어서 1024byte씩 메모리에 저장
        packet_buffer[i].type = htonl(TYPE_DATA);
        packet_buffer[i].seq = htonl(i);
        packet_buffer[i].size = htonl(read_len);
    }
    fclose(fp);

    printf("파일 로드 완료 (총 패킷 수: %d, 윈도우 크기: %d)\n", total_packets, WINDOW_SIZE);
    
    // ====================================================
    // 성능 측정용 변수 선언 및 전체 타이머 시작
    // ====================================================
    int total_resend_count = 0; 
    long long start_total_time = get_current_time_ms();

    int base = 0;
    int next_seq = 0;
    long long timer_start = 0;
    int timer_running = 0;
    int data_retries = 0; // 연속 타임아웃 방어벽 변수

    while (base < total_packets) {
        // 1. 윈도우 크기만큼 패킷 연속 전송
        while (next_seq < base + WINDOW_SIZE && next_seq < total_packets) {
            int p_size = HEADER_SIZE + ntohl(packet_buffer[next_seq].size);
            sendto(sock, &packet_buffer[next_seq], p_size, 0, (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
            printf("[WINDOW SEND] SEQ:%d 전송 (Window: [%d ~ %d])\n", next_seq, base, base + WINDOW_SIZE - 1);
            
            if (!timer_running) {
                timer_start = get_current_time_ms();
                timer_running = 1;
            }
            next_seq++;
        }

        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(sock, &reads);
        struct timeval tv;

        // 지능형 대기 (Dynamic Timeout) 설정
        if (timer_running) {
            long long elapsed = get_current_time_ms() - timer_start;
            long long remaining = TIMEOUT_MS - elapsed;
            
            if (remaining < 0) remaining = 0; 
            
            tv.tv_sec = remaining / 1000;
            tv.tv_usec = (remaining % 1000) * 1000;
        } else {  
            // 타이머가 돌지 않을 때도 select의 안전한 숨통을 위해 1초 기본 대기 설정
            tv.tv_sec = 1;
            tv.tv_usec = 0;
        }

        // 남은 시간만큼만 효율적으로 대기 (Polling 차단)
        int select_result = select(sock + 1, &reads, NULL, NULL, &tv);

        if (select_result > 0) {  
            AckPacket ack;
            struct sockaddr_in from_addr;
            socklen_t from_addr_sz = sizeof(from_addr);
            int ack_len = recvfrom(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&from_addr, &from_addr_sz);

            if (ack_len >= (int)sizeof(ack) && ntohl(ack.type) == TYPE_ACK) {
                int ack_seq = ntohl(ack.seq);
                
                if (ack_seq >= base) {
                    printf("[RCV ACK] 서버 응답 완료 ➔ SEQ:%d (윈도우 슬라이딩!)\n", ack_seq);
                    base = ack_seq + 1;
                    data_retries = 0; // 정상적인 진전이 있으므로 연속 타임아웃 카운트 초기화
                    
                    if (base == next_seq) {
                        timer_running = 0;
                    } else {
                        timer_start = get_current_time_ms(); // 누적 ACK 확인 후 타이머 갱신
                    }
                }
            }
        }

        // 3. Timeout 발생 시 전체 재전송 (Go-Back-N)
        if (timer_running && (get_current_time_ms() - timer_start >= TIMEOUT_MS)) {
            data_retries++;
            printf("\n[TIMEOUT] SEQ:%d 패킷 응답 시간 초과! Go-Back-N 유실 복구 작동...\n", base);
            
            // 연속 5회 타임아웃 발생 시 서버 다운으로 판단하고 안전 종료
            if (data_retries >= 5) {
                printf("[FATAL] 서버가 응답하지 않습니다. 데이터 전송을 강제 중단합니다.\n");
                free(packet_buffer);
                close(sock);
                exit(1); 
            }
            
            // GBN 재전송 루프
            for (int i = base; i < next_seq; i++) {
                int p_size = HEADER_SIZE + ntohl(packet_buffer[i].size);
                sendto(sock, &packet_buffer[i], p_size, 0, (struct sockaddr*)&receiver_addr, sizeof(receiver_addr));
                printf("[GBN RESEND] 패킷 복구 재전송 ➔ SEQ:%d\n", i);
                
                total_resend_count++; // 재전송이 일어날 때마다 지표 누적!
            }
            printf("\n");
            timer_start = get_current_time_ms();
        }
    }

    // 연결 종료 단계 (FIN 전송)
    if (!send_fin_with_retry(sock, base, &receiver_addr)) {
        printf("[WARN] FIN_ACK 수신을 결국 실패했으나, 최대 시도 초과로 강제 종료합니다.\n");
    }

    long long end_total_time = get_current_time_ms();
    
    // ====================================================
    //  [최종 성능 결과 리포트]
    // ====================================================
    double total_time_sec = (end_total_time - start_total_time) / 1000.0;
    
    printf("\n\n전송 완료!");
    printf("=============================================\n");
    printf("[최종 성능 평가 리포트]\n");
	printf(" 설정된 윈도우 크기 (WINDOW_SIZE) : %d\n", WINDOW_SIZE);
    printf(" 전송된 파일 크기 (File Size)    : %.2f MB\n", file_size / (1024.0 * 1024.0));
    printf(" 총 소요 시간 (Total Time)       : %.3f 초\n", total_time_sec);
    printf(" 총 패킷 재전송 횟수 (Resends)   : %d 회\n", total_resend_count);
    printf(" 시스템 처리량 (Throughput)      : %.2f MB/s\n", (file_size / total_time_sec) / (1024.0 * 1024.0));
    printf("=============================================\n");

    free(packet_buffer);
    close(sock);
    return 0;
}

long long get_current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int send_fin_with_retry(int sock, int fin_seq, struct sockaddr_in *receiver_addr) {
    Packet fin_packet;
    fin_packet.type = htonl(TYPE_FIN);
    fin_packet.seq = htonl(fin_seq);
    fin_packet.size = htonl(0);
    
    int fin_retries = 0;
    printf("\n===========================================\n");
    printf("[종료 시도] 모든 데이터 전송 완료 ➔ TYPE_FIN 전송 시작\n");
    
    while (fin_retries < 5) {
        sendto(sock, &fin_packet, HEADER_SIZE, 0, (struct sockaddr*)receiver_addr, sizeof(*receiver_addr));
        printf("[SEND FIN] TYPE_FIN 전송 (시도 %d/5) ➔ FIN_ACK 대기 중...\n", fin_retries + 1);
        
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(sock, &reads);
        struct timeval tv = {1, 0}; 
        
        int select_result = select(sock + 1, &reads, NULL, NULL, &tv);
        
        if (select_result > 0) {
            AckPacket ack;
            struct sockaddr_in from_addr;
            socklen_t from_addr_sz = sizeof(from_addr);
            int ack_len = recvfrom(sock, &ack, sizeof(ack), 0, (struct sockaddr*)&from_addr, &from_addr_sz);
            
            if (ack_len >= (int)sizeof(ack) && ntohl(ack.type) == TYPE_ACK) {
                int ack_seq = ntohl(ack.seq);
                if (ack_seq == fin_seq) {
                    printf("[RCV FIN_ACK] 서버로부터 최종 종료 확인 완료! (SEQ:%d)\n", ack_seq);
                    return 1; 
                }
            }
        } else {
            printf("[TIMEOUT] FIN_ACK 응답 타임아웃! FIN 패킷 재전송을 준비합니다.\n");
            fin_retries++;
        }
    }
    return 0; 
}
