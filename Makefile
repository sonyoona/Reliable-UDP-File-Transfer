# 컴파일러 및 옵션 설정
CC = gcc
CFLAGS = -Wall -Wextra -O2

# 최종 생성할 실행 파일 이름
TARGETS = serv cli

# 기본 타겟 (make 라고만 치면 둘 다 컴파일됨)
all: $(TARGETS)

# 서버 프로그램 컴파일 규칙
serv: file_transferserv.c file_transfer.h
	$(CC) $(CFLAGS) file_transferserv.c -o serv

# 클라이언트 프로그램 컴파일 규칙
cli: file_transfercli.c file_transfer.h
	$(CC) $(CFLAGS) file_transfercli.c -o cli

# 컴파일로 생성된 바이너리 파일들을 삭제하는 규칙 (make clean)
clean:
	rm -f $(TARGETS) received.txt test.txt