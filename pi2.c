#include <stdio.h>
#include <unistd.h>
#include <pigpio.h> // Third- party library(pigpio) 사용 
#include <math.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <string.h>

// M_PI 정의
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// GPIO 핀 번호 정의
#define TRIG_PIN 23  // 초음파 센서 TRIG 핀: GPIO 23
#define ECHO_PIN 24  // 초음파 센서 ECHO 핀: GPIO 24
#define MPU6050_ADDRESS 0x68 // MPU6050 I2C 주소
#define PORT 12345 // 서버 포트 번호

int i2c_handle; // I2C 핸들러
int sock = 0; // 소켓

// 초음파 센서 거리 측정 함수
float getDistance() {
    gpioWrite(TRIG_PIN, PI_LOW); // 트리거 핀을 LOW로 설정
    usleep(2); // 2 마이크로초 대기 
    gpioWrite(TRIG_PIN, PI_HIGH); // 트리거 핀을 HIGH로 설정하여 초음파 신호 전송
    usleep(10); // 10 마이크로초 대기
    gpioWrite(TRIG_PIN, PI_LOW); // ㅌ리거 핀을 다시 LOW로 설정

    while (gpioRead(ECHO_PIN) == PI_LOW); // 에코 핀이 HIGH가 될 때까지 대기
    uint32_t startTime = gpioTick(); // 신호가 반환되기 시작한 시간 기록

    while (gpioRead(ECHO_PIN) == PI_HIGH); // 에코 핀이 LOW가 될 때까지 대기
    uint32_t travelTime = gpioTick() - startTime; // 신호의 왕복 시간 계산

    // 거리 계산 (음속: 34300 cm/s) 
    float distance = travelTime / 58.0;
    return distance; // 거리 반환
}

// MPU6050 초기화 함수
void MPU6050_init() {
    i2c_handle = i2cOpen(1, MPU6050_ADDRESS, 0); // I2C 통신 열기
    if (i2c_handle < 0) {
        printf("MPU6050 초기화 실패\n"); // 초기화 실패 시 오류 메시지 출력
        return;
    }
    // MPU6050 전원 관리 레지스터 설정
    i2cWriteByteData(i2c_handle, 0x6B, 0x00);  // PWR_MGMT_1 레지스터를 0으로 설정 
}

// MPU6050 가속도 데이터 읽기 함수
void MPU6050_read(float* accelX, float* accelY, float* accelZ) {
    int16_t rawData[3];
    // 각 축 (X, Y, Z)의 가속도 데이터 읽기 (16비트 데이터로 읽어서 rawData 배열에 저장)
    rawData[0] = (i2cReadByteData(i2c_handle, 0x3B) << 8) | i2cReadByteData(i2c_handle, 0x3C);  // ACCEL_XOUT_H, ACCEL_XOUT_L
    rawData[1] = (i2cReadByteData(i2c_handle, 0x3D) << 8) | i2cReadByteData(i2c_handle, 0x3E);  // ACCEL_YOUT_H, ACCEL_YOUT_L
    rawData[2] = (i2cReadByteData(i2c_handle, 0x3F) << 8) | i2cReadByteData(i2c_handle, 0x40);  // ACCEL_ZOUT_H, ACCEL_ZOUT_L

    // 가속도 데이터를 16384로 나누어서 G 단위로 변환
    *accelX = rawData[0] / 16384.0;
    *accelY = rawData[1] / 16384.0;
    *accelZ = rawData[2] / 16384.0;
}

// 기울기 각도 계산 함수
float getTiltAngle(float x, float y, float z) {
    // x, y, z 축의 가속도 데이터와 atan2 함수를 이용하여 기울기 각도 계산
    float angle = atan2(y, sqrt(x * x + z * z)) * 180.0 / M_PI;
    return angle; // 기울기 각도 반환
}

// 메인 함수
int main(int argc, char const* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <server_ip>\n", argv[0]); // 사용법 출력
        return -1;
    }

    if (gpioInitialise() < 0) {
        printf("pigpio 초기화 실패\n"); // pigpio 초기화 실패 시 오류 메시지 출력
        return 1;
    }

    // GPIO 핀 모드 설정
    gpioSetMode(TRIG_PIN, PI_OUTPUT); // TRIG 핀을 출력 모드로 설정
    gpioSetMode(ECHO_PIN, PI_INPUT); // ECHO 핀을 입력 모드로 설정

    MPU6050_init(); // MPU6050 초기화

    struct sockaddr_in serv_addr; // 서버 주소 구조체
    char buffer[1024] = { 0 }; // 데이터 버퍼

    // 소켓 생성
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n"); // 소켓 생성 실패 시 오류 메시지 출력
        return -1;
    }

    serv_addr.sin_family = AF_INET; // IPv4 설정
    serv_addr.sin_port = htons(PORT); // 포트 번호 설정

    // IP 주소 설정
    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n"); // ㅇ효하지 않은 주소일 경우 오류 메시지 출력
        return -1;
    }

    // 서버에 연결
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n"); // 연결 실패 시 오류 메시지 출력
        return -1;
    }

    while (1) {
        float distance = getDistance(); // 초음파 센서로 거리 측정
        printf("거리: %.2f cm\n", distance); // 거리 출력

        float accelX, accelY, accelZ;
        MPU6050_read(&accelX, &accelY, &accelZ); // MPU6050 가속도 데이터 읽기
        float tiltAngle = getTiltAngle(accelX, accelY, accelZ); // 기울기 각도 계산
        printf("기울기: %.2f 도\n", tiltAngle); // 기울기 각도 출력

        // 거리와 기울기 각도를 문자열로 변환하여 서버로 전송
        snprintf(buffer, sizeof(buffer), "Distance: %.2f cm, Tilt Angle: %.2f degrees", distance, tiltAngle);
        send(sock, buffer, strlen(buffer), 0);

        usleep(100000); // 0.1초 대기
    }

    i2cClose(i2c_handle); // I2C 인터페이스 닫기
    gpioTerminate(); // GPIO 종료
    close(sock); // 소켓 닫기
    return 0;
}
