#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PORT 12345
#define BUF_SIZE 1024

#define IN1 5   // A-IA -> GPIO5
#define IN2 6   // A-IB -> GPIO6
#define IN3 13  // B-IA -> GPIO13
#define IN4 19  // B-IB -> GPIO19

static const char* DEVICE = "/dev/spidev0.0";
static uint8_t MODE = 0;
static uint8_t BITS = 8;
static uint32_t CLOCK = 1000000;
static uint16_t DELAY = 5;
static const char* GPIO_EXPORT = "/sys/class/gpio/export";
static const char* GPIO_UNEXPORT = "/sys/class/gpio/unexport";
static const char* GPIO_LED_PIN = "18"; // BCM �� ��ȣ
static char GPIO_LED_DIRECTION[50];
static char GPIO_LED_VALUE[50];

float Distance, Angle;
pthread_mutex_t lock;

// �Լ� ������Ÿ�� ����
void gpio_unexport(int pin);
void gpio_export(int pin);
void gpio_set_direction(int pin, const char* direction);
void gpio_write(int pin, int value);

void kill_process_using_port(int port) {
    char command[100];
    snprintf(command, sizeof(command), "fuser -k %d/tcp", port);
    system(command);
}

static int prepare(int fd) {
    if (ioctl(fd, SPI_IOC_WR_MODE, &MODE) == -1) {
        perror("Can't set MODE");
        return -1;
    }
    printf("MODE set successfully\n");

    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &BITS) == -1) {
        perror("Can't set number of BITS");
        return -1;
    }
    printf("BITS set successfully\n");

    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &CLOCK) == -1) {
        perror("Can't set write CLOCK");
        return -1;
    }
    printf("Write CLOCK set successfully\n");

    if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &CLOCK) == -1) {
        perror("Can't set read CLOCK");
        return -1;
    }
    printf("Read CLOCK set successfully\n");

    return 0;
}

uint8_t control_bits_differential(uint8_t channel) {
    return (channel & 7) << 4;
}

uint8_t control_bits(uint8_t channel) {
    return 0x8 | control_bits_differential(channel);
}

int readadc(int fd, uint8_t channel) {
    uint8_t tx[] = { 1, control_bits(channel), 0 };
    uint8_t rx[3];

    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = ARRAY_SIZE(tx),
        .delay_usecs = DELAY,
        .speed_hz = CLOCK,
        .bits_per_word = BITS,
    };

    if (ioctl(fd, SPI_IOC_MESSAGE(1), &tr) == -1) {
        perror("IO Error");
        abort();
    }

    return ((rx[1] << 8) & 0x300) | (rx[2] & 0xFF);
}

void gpio_export(int pin) {
    int fd = open(GPIO_EXPORT, O_WRONLY);
    if (fd == -1) {
        perror("GPIO export open error");
        exit(EXIT_FAILURE);
    }

    char buffer[3];
    int len = snprintf(buffer, sizeof(buffer), "%d", pin);
    if (write(fd, buffer, len) == -1) {
        if (errno == EBUSY) {
            printf("GPIO %d is already exported, unexporting...\n", pin);
            gpio_unexport(pin);  // �̹� �������� ��� ���� ����
            if (write(fd, buffer, len) == -1) {
                perror("GPIO export write error after unexport");
                exit(EXIT_FAILURE);
            }
        }
        else {
            perror("GPIO export write error");
            exit(EXIT_FAILURE);
        }
    }

    close(fd);
    printf("GPIO %d exported\n", pin);
}

void gpio_unexport(int pin) {
    int fd = open(GPIO_UNEXPORT, O_WRONLY);
    if (fd == -1) {
        perror("GPIO unexport open error");
        exit(EXIT_FAILURE);
    }

    char buffer[3];
    int len = snprintf(buffer, sizeof(buffer), "%d", pin);
    if (write(fd, buffer, len) == -1) {
        if (errno != EINVAL) {
            perror("GPIO unexport write error");
            exit(EXIT_FAILURE);
        }
        else {
            printf("GPIO %d is not currently exported\n", pin);
        }
    }
    else {
        printf("GPIO %d unexported\n", pin);
    }

    close(fd);
}

void gpio_set_direction(int pin, const char* direction) {
    char path[35];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);

    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("GPIO direction open error");
        exit(EXIT_FAILURE);
    }

    if (write(fd, direction, strlen(direction)) == -1) {
        perror("GPIO direction write error");
        exit(EXIT_FAILURE);
    }

    close(fd);
    printf("GPIO direction set to %s for pin %d\n", direction, pin);
}

void gpio_write(int pin, int value) {
    char path[30];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);

    int fd = open(path, O_WRONLY);
    if (fd == -1) {
        perror("GPIO value open error");
        exit(EXIT_FAILURE);
    }

    char buffer = value ? '1' : '0';
    if (write(fd, &buffer, 1) == -1) {
        perror("GPIO value write error");
        exit(EXIT_FAILURE);
    }

    close(fd);
    //printf("GPIO value set to %d for pin %d\n", value, pin);
}

void* spi_thread(void* arg) {
    int fd = open(DEVICE, O_RDWR);
    if (fd <= 0) {
        perror("Device open error");
        return NULL;
    }

    if (prepare(fd) == -1) {
        perror("Device prepare error");
        return NULL;
    }

    snprintf(GPIO_LED_DIRECTION, sizeof(GPIO_LED_DIRECTION), "/sys/class/gpio/gpio%s/direction", GPIO_LED_PIN);
    snprintf(GPIO_LED_VALUE, sizeof(GPIO_LED_VALUE), "/sys/class/gpio/gpio%s/value", GPIO_LED_PIN);

    // GPIO �ʱ� ����
    gpio_unexport(atoi(GPIO_LED_PIN)); // ���� ���� ����
    gpio_export(atoi(GPIO_LED_PIN)); // GPIO �� ��������
    gpio_set_direction(atoi(GPIO_LED_PIN), "out"); // GPIO �� ���� ����

    time_t last_led_check = time(NULL); // ������ LED Ȯ�� �ð� �ʱ�ȭ

    while (1) {
        int value = readadc(fd, 0);
        // printf("SPI value: %d\n", value);

         // ���� �ð� Ȯ��
        time_t current_time = time(NULL);

        // ������ LED Ȯ�� �ð����� 10�ʰ� ������ LED ���� Ȯ��
        if (current_time - last_led_check >= 10) {
            if (value <= 400) {
                gpio_write(atoi(GPIO_LED_PIN), 1); // LED �ѱ�
            }
            else {
                gpio_write(atoi(GPIO_LED_PIN), 0); // LED ����
            }

            // LED Ȯ�� �ð� ����
            last_led_check = current_time;
        }

        usleep(5000); // 5ms ���
    }

    close(fd);
    return NULL;
}

void set_motor(int motor, int speed) {
    int in1, in2;
    if (motor == 1) {
        in1 = IN1;
        in2 = IN2;
    }
    else if (motor == 2) {
        in1 = IN3;
        in2 = IN4;
    }
    else {
        return; // �߸��� ���� ��ȣ
    }

    int duty_cycle = abs(speed); // ��Ƽ ����Ŭ (0-100)

    for (int i = 0; i < 100; i++) {
        if (i < duty_cycle) {
            if (speed > 0) {
                gpio_write(in1, 1);
                gpio_write(in2, 0);
            }
            else {
                gpio_write(in1, 0);
                gpio_write(in2, 1);
            }
        }
        else {
            gpio_write(in1, 0);
            gpio_write(in2, 0);
        }
        usleep(50); // 50us �ֱ� (20kHz ���ļ�)
    }
}

void* socket_thread(void* arg) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[BUF_SIZE] = { 0 };
    int opt = 1;

    // ��Ʈ�� ��� ���� ���μ��� ����
    kill_process_using_port(PORT);

    // ���� ����
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return NULL;
    }

    // ���� �ɼ� ����: ��Ʈ ���� ����
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        return NULL;
    }

    // �ּ� ����ü ����
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    // ���Ͽ� �ּ� �Ҵ�
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return NULL;
    }

    // ���� ��� ���·� ��ȯ
    if (listen(server_fd, 3) < 0) {
        perror("listen failed");
        close(server_fd);
        return NULL;
    }

    printf("Server listening on port %d\n", PORT);

    // Ŭ���̾�Ʈ ���� ����
    if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
        perror("accept failed");
        close(server_fd);
        return NULL;
    }

    // GPIO �ʱ�ȭ
    gpio_export(IN1);
    gpio_export(IN2);
    gpio_export(IN3);
    gpio_export(IN4);

    gpio_set_direction(IN1, "out"); // ������ �������
    gpio_set_direction(IN2, "out"); // ������ �������
    gpio_set_direction(IN3, "out"); // ������ �������
    gpio_set_direction(IN4, "out"); // ������ �������

    while (1) {
        int valread = read(new_socket, buffer, BUF_SIZE);
        if (valread > 0) {
            buffer[valread] = '\0'; // null-terminate the buffer

            // ���ڿ����� Distance�� Angle �� ����
            pthread_mutex_lock(&lock);
            if (sscanf(buffer, "Distance: %f cm, Tilt Angle: %f degrees", &Distance, &Angle) == 2) {
                //        printf("Distance: %.2f cm, Angle: %.2f degrees\n", Distance, Angle);

                if (Distance < 10) {
                    set_motor(1, 0);
                    set_motor(2, 0);
                    //  printf("Motors stopped due to close distance.\n");
                }
                else if (Angle < -30 || Angle > 30) {
                    set_motor(1, 65);  // 65% �ӵ�
                    set_motor(2, 65);  // 65% �ӵ�
                    //  printf("Motors running at reduced speed due to angle.\n");
                }
                else if (Angle < -15 || Angle > 15) {
                    set_motor(1, 75);  // 75% �ӵ�
                    set_motor(2, 75);  // 75% �ӵ�
                    //  printf("Motors running at moderate speed due to angle.\n");
                }
                else {
                    set_motor(1, 95);  // ���� �ӵ�
                    set_motor(2, 95);  // ���� �ӵ�
                    //  printf("Motors running normally.\n");
                }
            }
            else {
                printf("Failed to parse the received string.\n");
            }
            pthread_mutex_unlock(&lock);
        }
        usleep(5000); // 5ms ���
    }

    // Ŭ���̾�Ʈ ���ϰ� ���� ���� �ݱ�
    close(new_socket);
    close(server_fd);

    // GPIO ����
    gpio_unexport(IN1);
    gpio_unexport(IN2);
    gpio_unexport(IN3);
    gpio_unexport(IN4);

    return NULL;
}

int main() {
    pthread_t spi_tid, socket_tid;

    // ���ؽ� �ʱ�ȭ
    if (pthread_mutex_init(&lock, NULL) != 0) {
        perror("pthread_mutex_init failed");
        return 1;
    }

    // ������ ����
    if (pthread_create(&spi_tid, NULL, spi_thread, NULL) != 0) {
        perror("pthread_create for spi_thread failed");
        return 1;
    }

    if (pthread_create(&socket_tid, NULL, socket_thread, NULL) != 0) {
        perror("pthread_create for socket_thread failed");
        return 1;
    }

    // ������ ���� ���
    pthread_join(spi_tid, NULL);
    pthread_join(socket_tid, NULL);

    // ���ؽ� ����
    pthread_mutex_destroy(&lock);

    return 0;
}
