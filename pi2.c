#include <stdio.h>
#include <unistd.h>
#include <pigpio.h> // Third- party library(pigpio) ��� 
#include <math.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// �� ����
#define TRIG_PIN 23  // GPIO 23
#define ECHO_PIN 24  // GPIO 24
#define MPU6050_ADDRESS 0x68
#define PORT 12345

int i2c_handle;
int sock = 0;

float getDistance() {
    gpioWrite(TRIG_PIN, PI_LOW);
    usleep(2);
    gpioWrite(TRIG_PIN, PI_HIGH);
    usleep(10);
    gpioWrite(TRIG_PIN, PI_LOW);

    while (gpioRead(ECHO_PIN) == PI_LOW);
    uint32_t startTime = gpioTick();

    while (gpioRead(ECHO_PIN) == PI_HIGH);
    uint32_t travelTime = gpioTick() - startTime;

    // �Ÿ� ��� (����: 34300 cm/s)
    float distance = travelTime / 58.0;
    return distance;
}

void MPU6050_init() {
    i2c_handle = i2cOpen(1, MPU6050_ADDRESS, 0);
    if (i2c_handle < 0) {
        printf("MPU6050 �ʱ�ȭ ����\n");
        return;
    }
    // MPU6050 �ʱ� ����
    i2cWriteByteData(i2c_handle, 0x6B, 0x00);  // PWR_MGMT_1 �������͸� 0���� ����
}

void MPU6050_read(float* accelX, float* accelY, float* accelZ) {
    int16_t rawData[3];
    rawData[0] = (i2cReadByteData(i2c_handle, 0x3B) << 8) | i2cReadByteData(i2c_handle, 0x3C);  // ACCEL_XOUT_H, ACCEL_XOUT_L
    rawData[1] = (i2cReadByteData(i2c_handle, 0x3D) << 8) | i2cReadByteData(i2c_handle, 0x3E);  // ACCEL_YOUT_H, ACCEL_YOUT_L
    rawData[2] = (i2cReadByteData(i2c_handle, 0x3F) << 8) | i2cReadByteData(i2c_handle, 0x40);  // ACCEL_ZOUT_H, ACCEL_ZOUT_L

    *accelX = rawData[0] / 16384.0;
    *accelY = rawData[1] / 16384.0;
    *accelZ = rawData[2] / 16384.0;
}

float getTiltAngle(float x, float y, float z) {
    float angle = atan2(y, sqrt(x * x + z * z)) * 180.0 / M_PI;
    return angle;
}

int main(int argc, char const* argv[]) {
    if (argc != 2) {
        printf("Usage: %s <server_ip>\n", argv[0]);
        return -1;
    }

    if (gpioInitialise() < 0) {
        printf("pigpio �ʱ�ȭ ����\n");
        return 1;
    }

    gpioSetMode(TRIG_PIN, PI_OUTPUT);
    gpioSetMode(ECHO_PIN, PI_INPUT);

    MPU6050_init();

    struct sockaddr_in serv_addr;
    char buffer[1024] = { 0 };

    // ���� ����
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("\n Socket creation error \n");
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);

    // IP �ּ� ����
    if (inet_pton(AF_INET, argv[1], &serv_addr.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }

    // ������ ����
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("\nConnection Failed \n");
        return -1;
    }

    while (1) {
        float distance = getDistance();
        printf("�Ÿ�: %.2f cm\n", distance);

        float accelX, accelY, accelZ;
        MPU6050_read(&accelX, &accelY, &accelZ);
        float tiltAngle = getTiltAngle(accelX, accelY, accelZ);
        printf("����: %.2f ��\n", tiltAngle);

        snprintf(buffer, sizeof(buffer), "Distance: %.2f cm, Tilt Angle: %.2f degrees", distance, tiltAngle);
        send(sock, buffer, strlen(buffer), 0);

        usleep(100000); // 0.1�� ���
    }

    i2cClose(i2c_handle);
    gpioTerminate();
    close(sock);
    return 0;
}
