#include <gpiod.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "oled_i2c.h"
#include "getip.h"

#define DEBOUNCE_DELAY_US 1000
#define DEBOUNCE_COUNT 1
#define MAX_MONITORS 3
#define NUM_KEYS 3
#define FONT_PATH "./fonts/NotoSerifThai.ttf"
#define FONT_SIZE 24
#define PING_TIMEOUT_US 500000  // 0.5s

// GPIO
static struct gpiod_chip *chip;
static struct gpiod_line *do_sw;
static struct gpiod_line *btn_down;
static struct gpiod_line *btn_up;
static struct gpiod_line *btn_done;
static struct gpiod_line *monitor_keys[NUM_KEYS];

// GPIO LED
static struct gpiod_line *led_red;
static struct gpiod_line *led_yellow;
static struct gpiod_line *led_green;

int gpio_lines[NUM_KEYS] = {7,8,9};  // monitor1-3
const char *key_names[NUM_KEYS] = {"MON1","MON2","MON3"};

// Network
static int sockfd;
static struct sockaddr_in dest_addr;
char *monitor_ips[MAX_MONITORS];
int monitor_port;
int current_monitor = 0;

int combo_start = 0;
time_t combo_active = 0;

// FreeType
FT_Library ft;
FT_Face face;

// Signal handler
void intHandler(int dummy){
    // ปิด LED ทั้งหมดก่อนออก
    gpiod_line_set_value(led_red, 0);
    gpiod_line_set_value(led_yellow, 0);
    gpiod_line_set_value(led_green, 0);

    gpiod_line_release(do_sw);
    gpiod_line_release(btn_down);
    gpiod_line_release(btn_up);
    gpiod_line_release(btn_done);
    for(int i=0;i<NUM_KEYS;i++) gpiod_line_release(monitor_keys[i]);
    gpiod_chip_close(chip);
    close(sockfd);
    oled_clear();
    oled_display();
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
    printf("\nExiting safely.\n");
    exit(0);
}

// โหลด config จาก env
void load_env_config() {
    FILE *fp = fopen(".env", "r");
    if (!fp) {
        perror("fopen .env");
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        if (!key || !value) continue;

        // ตัดช่องว่าง
        while (*value == ' ') value++;

        if (strcmp(key, "MONITOR_IP1") == 0) monitor_ips[0] = strdup(value);
        else if (strcmp(key, "MONITOR_IP2") == 0) monitor_ips[1] = strdup(value);
        else if (strcmp(key, "MONITOR_IP3") == 0) monitor_ips[2] = strdup(value);
        else if (strcmp(key, "MONITOR_PORT") == 0) monitor_port = atoi(value);
    }
    fclose(fp);

    // ค่า default ถ้าไม่มีใน .env
    if (!monitor_ips[0]) monitor_ips[0] = strdup("192.168.1.111");
    if (!monitor_ips[1]) monitor_ips[1] = strdup("192.168.1.111");
    if (!monitor_ips[2]) monitor_ips[2] = strdup("192.168.1.130");
    if (!monitor_port) monitor_port = 5000;
}
// ตั้ง monitor ปัจจุบัน
void set_monitor(int idx) {
    if(idx < 0 || idx >= MAX_MONITORS) return;

    current_monitor = idx;
    memset(&dest_addr,0,sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(monitor_port);
    if(inet_pton(AF_INET, monitor_ips[current_monitor], &dest_addr.sin_addr)<=0){
        perror("Invalid address");
        exit(1);
    }
    printf("Switched to monitor%d (%s:%d)\n",current_monitor+1,monitor_ips[current_monitor],monitor_port);
}

// debounce
int read_line_debounced(struct gpiod_line *line){
    int last_val = gpiod_line_get_value(line);
    if(last_val < 0) return last_val;
    int stable_count = 0;
    while(stable_count < DEBOUNCE_COUNT){
        usleep(DEBOUNCE_DELAY_US);
        int val = gpiod_line_get_value(line);
        if(val < 0) return val;
        if(val == last_val) stable_count++;
        else { stable_count=0; last_val=val; }
    }
    return last_val;
}

void render_monitor_text(const char *line1, const char *line2, int font_size) {
    // ล้างหน้าจอ
    oled_clear();

    // ตั้งขนาดฟอนต์
    FT_Set_Pixel_Sizes(face, 0, font_size);

    // แสดงข้อความ
    render_text(line1, 0, 30, face); // บรรทัดบน
    render_text(line2, 0, 60, face); // บรรทัดล่าง

    // แสดงผลบน OLED
    oled_display();
}

// ตรวจสอบ monitor ด้วย ping/pong
int check_monitor(){
    struct sockaddr_in addr = dest_addr;
    char buf[16];
    fd_set readfds;
    struct timeval tv;
    int ret;

    const char *msg = "ping";
    sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&addr,sizeof(addr));

    FD_ZERO(&readfds);
    FD_SET(sockfd,&readfds);
    tv.tv_sec = 0;
    tv.tv_usec = PING_TIMEOUT_US;

    ret = select(sockfd+1,&readfds,NULL,NULL,&tv);
    if(ret > 0 && FD_ISSET(sockfd,&readfds)){
        socklen_t len = sizeof(addr);
        int n = recvfrom(sockfd,buf,sizeof(buf)-1,0,(struct sockaddr*)&addr,&len);
        if(n>0){ buf[n]=0; if(strcmp(buf,"pong")==0) return 1; }
    }
    return 0;
}

// แสดง OLED + เช็ค connection
void display_monitor_status(int idx){
    char buf[32];
    snprintf(buf,sizeof(buf),"หน้าจอ: %d",idx+1);
    int connected = check_monitor();
    char buf2[32];
    snprintf(buf2,sizeof(buf2),connected?"เชื่อมต่อ":"            ");
    render_monitor_text(buf,buf2,FONT_SIZE);
}

int main(){
    setvbuf(stdout, NULL, _IOLBF, 0);   // line-buffered stdout
    setvbuf(stderr, NULL, _IONBF, 0);   // unbuffered stderr

    printf("monitor_control starting...\n");
    fflush(stdout);

    signal(SIGINT,intHandler);
    load_env_config();

    chip = gpiod_chip_open_by_name("gpiochip0");
    if(!chip){ perror("Open chip failed"); return 1; }


    led_red    = gpiod_chip_get_line(chip, 0); // R
    led_yellow = gpiod_chip_get_line(chip, 2); // Y
    led_green  = gpiod_chip_get_line(chip, 3); // G

    if (!led_red || !led_yellow || !led_green) {
        perror("Get LED line failed");
        return 1;
    }

    if (gpiod_line_request_output(led_red, "led_red", 0) < 0) {
        perror("Request output LED R failed");
        return 1;
    }
    if (gpiod_line_request_output(led_yellow, "led_yellow", 0) < 0) {
        perror("Request output LED Y failed");
        return 1;
    }
    if (gpiod_line_request_output(led_green, "led_green", 0) < 0) {
        perror("Request output LED G failed");
        return 1;
    }

    // ปุ่ม DO
    do_sw = gpiod_chip_get_line(chip,6);
    if(!do_sw){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(do_sw,"sw_do")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม DOWN
    btn_down = gpiod_chip_get_line(chip,21);
    if(!btn_down){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(btn_down,"sw_down")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม UP (GPIOA20 / PCM0_DOUT)
    btn_up = gpiod_chip_get_line(chip,20);
    if(!btn_up){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(btn_up,"sw_up")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม DONE (SPDIF-OUT / GPIOA17)
    btn_done = gpiod_chip_get_line(chip,17);
    if(!btn_done){ perror("Get line failed"); return 1; }
    if(gpiod_line_request_input(btn_done,"sw_done")<0){ perror("Request input failed"); return 1; }

    // ปุ่ม monitor
    for(int i=0;i<NUM_KEYS;i++){
        monitor_keys[i]=gpiod_chip_get_line(chip,gpio_lines[i]);
        if(!monitor_keys[i]){ perror("Get line failed"); return 1; }
        if(gpiod_line_request_input(monitor_keys[i],"monitor_keys")<0){ perror("Request input failed"); return 1; }
    }

    // socket UDP
    sockfd=socket(AF_INET,SOCK_DGRAM,0);
    if(sockfd<0){ perror("Socket failed"); return 1; }

    set_monitor(current_monitor);

    // OLED + FreeType
    oled_init();
    if (FT_Init_FreeType(&ft)) { fprintf(stderr,"Could not init FreeType\n"); return 1; }
    if (FT_New_Face(ft,FONT_PATH,0,&face)) { fprintf(stderr,"Failed to load font\n"); return 1; }
    FT_Set_Pixel_Sizes(face,0,FONT_SIZE);

    display_monitor_status(current_monitor); // แสดง monitor เริ่มต้น + check

    int last_do_state=1;
    int last_down_state=1;
    int last_up_state=1;
    int last_done_state=1;
    int last_monitor_state[NUM_KEYS]={1,1,1};

    time_t last_check = 0;

    time_t shutdown_start = 0;  // เก็บเวลาที่เริ่มกด 3 ปุ่ม
    int led_blink_state = 0;    // สำหรับกระพริบ LED
    time_t last_blink = 0;      // เก็บเวลาล่าสุดที่สลับไฟ

    gpiod_line_set_value(led_red, 1);

    while(1){
        // ปุ่ม DO
        int val_do=read_line_debounced(do_sw);
        if(val_do==0 && last_do_state!=0){
            printf("DO pressed! Sending 'do' to monitor %d\n",current_monitor+1);
            const char *msg="do";
            if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                perror("Send failed");

            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            render_monitor_text(buf,"ทำรายการ", FONT_SIZE);
        }
        last_do_state=val_do;

        int val_down = read_line_debounced(btn_down);  // 0 = pressed
        int val_up   = read_line_debounced(btn_up);    // 0 = pressed

// ตรวจว่ากดพร้อมกันหรือไม่
if (val_down == 1 && val_up == 1) {
    if (!combo_active) {
        // เพิ่งกดพร้อมกันครั้งแรก
        combo_start = time(NULL);
        combo_active = 1;
    } else {
        // กำลังกดค้าง
        if (time(NULL) - combo_start >= 3) {
            char ip[64];
            if (get_ip_address("eth0", ip, sizeof(ip)) == 0) {
                printf("📡 IP Address: %s\n", ip);
                render_monitor_text("IP Address", ip, 18);
            } else {
                render_monitor_text("IP Address", "Error", 18);
            }
            combo_active = 0;   // reset ไม่ให้โชว์ซ้ำ
        }
    }
} else {
    // ปล่อยปุ่ม
    combo_active = 0;
    combo_start = 0;
}

// ====== โค้ดปุ่ม DOWN ปกติ ======
if (val_down==1 && last_down_state!=1){
    printf("DOWN pressed! Sending 'down' to monitor %d\n",current_monitor+1);
    const char *msg="down";
    if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
        perror("Send failed");

    char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
    render_monitor_text(buf,"ลง", FONT_SIZE);
}
last_down_state=val_down;

// ====== โค้ดปุ่ม UP ปกติ ======
if (val_up==1 && last_up_state!=1){
    printf("UP pressed! Sending 'up' to monitor %d\n",current_monitor+1);
    const char *msg="up";
    if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
        perror("Send failed");

    char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
    render_monitor_text(buf,"ขึ้น", FONT_SIZE);
}
last_up_state=val_up;//                perror("Send failed");

//            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
//            render_monitor_text(buf,"ขึ้น");
//        }
//        last_up_state = val_up;



        // ปุ่ม DONE
        int val_done = read_line_debounced(btn_done);
        if(val_done==1 && last_done_state!=1){
            printf("DONE pressed! Sending 'done' to monitor %d\n",current_monitor+1);
            const char *msg="done";
            if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                perror("Send failed");

            char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            render_monitor_text(buf,"เสร็จ", FONT_SIZE);
        }
        last_done_state = val_done;

        // ปุ่ม monitor
        for(int i=0;i<NUM_KEYS;i++){
            int val=read_line_debounced(monitor_keys[i]);
            if(val==0 && last_monitor_state[i]!=0){
                set_monitor(i);
                display_monitor_status(i);

                // ส่งข้อความ UDP ไป monitor ที่เลือก
                char msg[4];
                snprintf(msg,sizeof(msg),"m%d",i+1);
                if(sendto(sockfd,msg,strlen(msg),0,(struct sockaddr*)&dest_addr,sizeof(dest_addr))<0)
                    perror("Send failed");

                printf("เลือก monitor%d\n", i+1);

                // แสดงผลบน OLED
                char buf[32]; snprintf(buf,sizeof(buf),"หน้าจอ: %d",i+1);
                render_monitor_text(buf,"เลือกจอ", FONT_SIZE);

                // ✅ ควบคุม LED ตาม monitor
                gpiod_line_set_value(led_red,   i==0 ? 1 : 0);   // monitor1 -> R
                gpiod_line_set_value(led_yellow,i==1 ? 1 : 0);   // monitor2 -> Y
                gpiod_line_set_value(led_green, i==2 ? 1 : 0);   // monitor3 -> G
            }
            last_monitor_state[i]=val;
        }

// ปุ่ม monitor
int pressed_all = 1;
for(int i=0;i<NUM_KEYS;i++){
    int val = read_line_debounced(monitor_keys[i]);
    if(val != 0) pressed_all = 0;
    last_monitor_state[i] = val;
}

if(pressed_all){
    if(shutdown_start == 0){
        shutdown_start = time(NULL);  // เริ่มจับเวลา
        render_monitor_text("Hold 3s","เพื่อปิดเครื่อง", 18);  // แสดงบน OLED
        last_blink = shutdown_start;
    } else {
        time_t now = time(NULL);

        // กระพริบ LED ทุก 0.5 วินาที
        if(difftime(now, last_blink) >= 0.5){
            led_blink_state = !led_blink_state;
            gpiod_line_set_value(led_red, led_blink_state);
            gpiod_line_set_value(led_yellow, led_blink_state);
            gpiod_line_set_value(led_green, led_blink_state);
            last_blink = now;
        }

        // ถ้าค้าง >= 3 วินาที -> Shutdown
        if(difftime(now, shutdown_start) >= 3.0){
            printf("กดปุ่มทั้ง 3 พร้อมกันค้าง 3 วินาที -> Shutdown\n");
            render_monitor_text("Shutdown","กำลังปิดเครื่อง", 18);
            system("shutdown -h now");
            return 0;
        }
    }
} else {
if(shutdown_start != 0){
    // reset ถ้ามีปุ่มปล่อย
    shutdown_start = 0;

    // ปิด LED กระพริบ -> กลับปกติ
    gpiod_line_set_value(led_red, 0);
    gpiod_line_set_value(led_yellow,0);
    gpiod_line_set_value(led_green, 0);

    // ล้างข้อความ OLED
    render_monitor_text("","", FONT_SIZE);  

    // ✅ ตั้ง monitor1 เป็นค่าเริ่มต้น
    set_monitor(0);
    display_monitor_status(0);

    // ปรับ LED ตาม monitor1
    gpiod_line_set_value(led_red,   1);   // monitor1 -> R
    gpiod_line_set_value(led_yellow,0);
    gpiod_line_set_value(led_green, 0);

    // แสดงบน OLED ว่าเลือก monitor1
    render_monitor_text("หน้าจอ: 1","เลือกจอ",FONT_SIZE);
}

}

        // ตรวจสอบ connection ทุก 5 วินาที
        if(difftime(time(NULL), last_check) >= 5){
            oled_clear_line(56, 16); // เคลียร์แถวล่าง

            char buf[32];
            snprintf(buf,sizeof(buf),"หน้าจอ: %d",current_monitor+1);
            int connected = check_monitor();
            char buf2[32];
            snprintf(buf2,sizeof(buf2),connected?"เชื่อมต่อ":"            ");
            render_monitor_text(buf,buf2, FONT_SIZE);

            last_check = time(NULL);
        }

        usleep(20000);
    }

    // Clean FreeType
    FT_Done_Face(face);
    FT_Done_FreeType(ft);

    return 0;
}
